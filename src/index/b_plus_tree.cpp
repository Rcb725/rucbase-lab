// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "b_plus_tree.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <memory>

#include "common/errors.h"
#include "index_scan.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

int compare_index_key(const char* left, const char* right, ColType type, int column_length) {
    if (left == nullptr || right == nullptr || column_length <= 0) {
        throw InternalError("Invalid index comparison input");
    }
    switch (type) {
        case TYPE_INT: {
            if (column_length != static_cast<int>(sizeof(int))) {
                throw InternalError("Invalid integer index key length");
            }
            int left_value;
            int right_value;
            memcpy(&left_value, left, sizeof(left_value));
            memcpy(&right_value, right, sizeof(right_value));
            return (left_value < right_value) ? -1 : ((left_value > right_value) ? 1 : 0);
        }
        case TYPE_FLOAT: {
            if (column_length != static_cast<int>(sizeof(float))) {
                throw InternalError("Invalid float index key length");
            }
            float left_value;
            float right_value;
            memcpy(&left_value, left, sizeof(left_value));
            memcpy(&right_value, right, sizeof(right_value));
            return (left_value < right_value) ? -1 : ((left_value > right_value) ? 1 : 0);
        }
        case TYPE_STRING:
            return memcmp(left, right, column_length);
        default:
            throw InternalError("Unexpected data type");
    }
}

int compare_index_key(const char* left,
                      const char* right,
                      const std::vector<ColType>& column_types,
                      const std::vector<int>& column_lengths) {
    if (column_types.size() != column_lengths.size()) {
        throw InternalError("Invalid composite index metadata");
    }
    int offset = 0;
    for (size_t i = 0; i < column_types.size(); ++i) {
        int result = compare_index_key(left + offset, right + offset, column_types[i], column_lengths[i]);
        if (result != 0) return result;
        offset += column_lengths[i];
    }
    return 0;
}

BPlusTreeNode::BPlusTreeNode(const IndexFileHeader* file_header, Page* page) : file_header_(file_header), page_(page) {
    page_header_ = reinterpret_cast<IndexPageHeader*>(page_->get_data());
    keys_ = page_->get_data() + sizeof(IndexPageHeader);
    rids_ = reinterpret_cast<Rid*>(keys_ + file_header_->keys_region_size_);
}

page_id_t BPlusTreeNode::get_page_no() const noexcept { return page_->get_page_id().page_no; }

PageId BPlusTreeNode::get_page_id() const noexcept { return page_->get_page_id(); }

page_id_t BPlusTreeNode::remove_and_return_only_child() {
    assert(get_size() == 1);
    page_id_t child_page_no = value_at(0);
    erase_pair(0);
    assert(get_size() == 0);
    return child_page_no;
}

int BPlusTreeNode::find_child(const BPlusTreeNode* child) const {
    int child_index = 0;
    while (child_index < page_header_->key_count && get_rid(child_index)->page_no != child->get_page_no()) {
        ++child_index;
    }
    assert(child_index < page_header_->key_count);
    return child_index;
}

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int BPlusTreeNode::lower_bound(const char* target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        const int middle = left + (right - left) / 2;
        if (compare_index_key(get_key(middle), target, file_header_->column_types_, file_header_->column_lengths_) < 0) {
            left = middle + 1;
        } else {
            right = middle;
        }
    }
    return left;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[0,num_key]，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 内部结点用它定位孩子时，返回 0 表示 target 比第一个 key 还小（只会出现在最左侧路径上）。
 */
int BPlusTreeNode::upper_bound(const char* target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        const int middle = left + (right - left) / 2;
        if (compare_index_key(get_key(middle), target, file_header_->column_types_, file_header_->column_lengths_) <= 0) {
            left = middle + 1;
        } else {
            right = middle;
        }
    }
    return left;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool BPlusTreeNode::leaf_lookup(const char* key, Rid** value) {
    const int pos = lower_bound(key);
    if (pos >= get_size() ||
        compare_index_key(get_key(pos), key, file_header_->column_types_, file_header_->column_lengths_) != 0) {
        return false;
    }
    if (value != nullptr) {
        *value = get_rid(pos);
    }
    return true;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t BPlusTreeNode::internal_lookup(const char* key) {
    assert(!is_leaf_page() && get_size() > 0);
    const int pos = upper_bound(key);
    const int child_idx = pos == 0 ? 0 : pos - 1;
    return get_rid(child_idx)->page_no;
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param key 连续键数组的起始地址。
 * @param rid 连续 Rid 数组的起始地址。
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void BPlusTreeNode::insert_pairs(int pos, const char* key, const Rid* rid, int n) {
    if (pos < 0 || pos > get_size() || n < 0 || (n > 0 && (key == nullptr || rid == nullptr)) ||
        get_size() + n > get_max_size()) {
        throw InternalError("invalid B+ tree node insertion");
    }
    if (n == 0) {
        return;
    }

    memmove(get_key(pos + n), get_key(pos),
            static_cast<size_t>(get_size() - pos) * file_header_->key_length_);
    memmove(get_rid(pos + n), get_rid(pos), static_cast<size_t>(get_size() - pos) * sizeof(Rid));
    memcpy(get_key(pos), key, static_cast<size_t>(n) * file_header_->key_length_);
    memcpy(get_rid(pos), rid, static_cast<size_t>(n) * sizeof(Rid));
    set_size(get_size() + n);
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param key 要插入的键。
 * @param value 要插入的 Rid。
 * @return int 键值对数量
 */
int BPlusTreeNode::insert(const char* key, const Rid& value) {
    const int pos = upper_bound(key);
    insert_pairs(pos, key, &value, 1);
    return get_size();
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void BPlusTreeNode::erase_pair(int pos) {
    if (pos < 0 || pos >= get_size()) {
        throw InternalError("invalid B+ tree node erase position");
    }
    memmove(get_key(pos), get_key(pos + 1),
            static_cast<size_t>(get_size() - pos - 1) * file_header_->key_length_);
    memmove(get_rid(pos), get_rid(pos + 1), static_cast<size_t>(get_size() - pos - 1) * sizeof(Rid));
    set_size(get_size() - 1);
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int BPlusTreeNode::remove(const char* key, const Rid& rid) {
    int pos = lower_bound(key);
    while (pos < get_size() &&
           compare_index_key(get_key(pos), key, file_header_->column_types_, file_header_->column_lengths_) == 0) {
        if (*get_rid(pos) == rid) {
            erase_pair(pos);
            return get_size();
        }
        ++pos;
    }
    return get_size();
}

BPlusTree::BPlusTree(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, int file_descriptor)
    : disk_manager_(disk_manager),
      buffer_pool_manager_(buffer_pool_manager),
      file_descriptor_(file_descriptor),
      file_header_() {
    alignas(int) char buf[PAGE_SIZE]{};
    disk_manager_->read_page(file_descriptor_, INDEX_FILE_HEADER_PAGE, buf, PAGE_SIZE);
    auto file_header = std::make_unique<IndexFileHeader>();
    file_header->deserialize(buf);

    // page_count_ is not a physical high-water mark after deletion or a crash.
    // Never reuse a page that is already present in the file.
    int64_t file_size = disk_manager_->get_file_size(file_descriptor_);
    if (file_size < 0) {
        throw InternalError("BPlusTree: cannot stat index file");
    }
    int64_t physical_pages_64 = file_size / PAGE_SIZE + (file_size % PAGE_SIZE != 0);
    if (physical_pages_64 > std::numeric_limits<page_id_t>::max()) {
        throw InternalError("BPlusTree: index file is too large");
    }
    auto physical_pages = static_cast<page_id_t>(physical_pages_64);
    disk_manager_->set_fd2pageno(file_descriptor_, std::max(file_header->page_count_, physical_pages));
    file_header_ = std::move(file_header);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @param find_first 是否从叶子链表的最左端开始查找
 * @return 目标叶子结点（IndexNode 离开作用域即 unpin）以及根结点是否加锁
 */
std::pair<IndexNode, bool> BPlusTree::find_leaf_page(const char* key,
                                                     IndexOperation operation,
                                                     Transaction* transaction,
                                                     bool find_first) {
    (void)operation;
    (void)transaction;
    if (is_empty()) {
        return {IndexNode{}, false};
    }

    page_id_t page_no = find_first ? file_header_->first_leaf_ : file_header_->root_page_;
    IndexNode node = fetch_node(page_no);
    while (node.valid() && !node->is_leaf_page()) {
        page_no = node->internal_lookup(key);
        node = fetch_node(page_no);
    }
    return {std::move(node), false};
}

/**
 * @brief 查找指定 key 对应的全部 Rid。
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 * @note 非唯一索引中，同一 key 的记录可能因分裂分布在多个相邻叶子里。
 */
bool BPlusTree::get_value(const char* key, std::vector<Rid>* result, Transaction* transaction) {
    (void)transaction;
    if (result == nullptr || key == nullptr) {
        throw InternalError("invalid B+ tree lookup argument");
    }
    std::scoped_lock lock{root_latch_};
    return get_value_impl(key, result);
}

bool BPlusTree::get_value_impl(const char* key, std::vector<Rid>* result) {
    if (is_empty()) {
        return false;
    }

    auto [leaf, root_latched] = find_leaf_page(key, IndexOperation::Find, nullptr);
    (void)root_latched;
    if (!leaf.valid()) {
        return false;
    }

    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE) {
        IndexNode previous = fetch_node(leaf->get_prev_leaf());
        if (!previous.valid() || previous->get_size() == 0 ||
            compare_index_key(previous->get_key(previous->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) < 0) {
            break;
        }
        leaf = std::move(previous);
    }

    bool found = false;
    while (leaf.valid()) {
        for (int i = 0; i < leaf->get_size(); ++i) {
            const int cmp = compare_index_key(leaf->get_key(i), key, file_header_->column_types_,
                                              file_header_->column_lengths_);
            if (cmp == 0) {
                result->push_back(*leaf->get_rid(i));
                found = true;
            } else if (cmp > 0) {
                return found;
            }
        }
        if (leaf->get_next_leaf() == INDEX_LEAF_HEADER_PAGE) {
            break;
        }
        IndexNode next = fetch_node(leaf->get_next_leaf());
        leaf = std::move(next);
    }
    return found;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note 返回的 IndexNode 拥有新页的 pin；原 node 仍由调用方持有。
 */
IndexNode BPlusTree::split(IndexNode& node) {
    IndexNode new_node = create_node();
    if (!new_node.valid()) {
        throw InternalError("buffer pool has no free page for B+ tree split");
    }

    const int old_size = node->get_size();
    const int left_size = (old_size + 1) / 2;
    const int right_size = old_size - left_size;
    new_node->page_header_->parent_page = node->get_parent_page_no();
    new_node->page_header_->key_count = 0;
    new_node->page_header_->is_leaf = node->is_leaf_page();
    new_node->page_header_->previous_leaf_page = INDEX_NO_PAGE;
    new_node->page_header_->next_leaf_page = INDEX_NO_PAGE;
    new_node->insert_pairs(0, node->get_key(left_size), node->get_rid(left_size), right_size);
    node->set_size(left_size);

    if (node->is_leaf_page()) {
        const page_id_t old_next = node->get_next_leaf();
        new_node->set_prev_leaf(node->get_page_no());
        new_node->set_next_leaf(old_next);
        node->set_next_leaf(new_node->get_page_no());

        IndexNode next = fetch_node(old_next);
        if (!next.valid()) {
            throw InternalError("failed to update leaf link after split");
        }
        next->set_prev_leaf(new_node->get_page_no());
        next.mark_dirty();
        if (file_header_->last_leaf_ == node->get_page_no()) {
            file_header_->last_leaf_ = new_node->get_page_no();
        }
    } else {
        for (int i = 0; i < right_size; ++i) {
            update_child_parent(new_node, i);
        }
    }

    node.mark_dirty();
    new_node.mark_dirty();
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param old_node 分裂后保留左半部分的原结点。
 * @param key 要插入parent的key
 * @param new_node 分裂产生的右兄弟结点。
 * @param transaction 当前事务；不需要并发控制时可为 nullptr。
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note old_node / new_node 的 pin 仍由调用方的 IndexNode 持有。
 */
void BPlusTree::insert_into_parent(IndexNode& old_node,
                                   const char* key,
                                   IndexNode& new_node,
                                   Transaction* transaction) {
    (void)transaction;
    new_node->set_parent_page_no(old_node->get_parent_page_no());

    if (old_node->is_root_page()) {
        IndexNode new_root = create_node();
        if (!new_root.valid()) {
            throw InternalError("buffer pool has no free page for new B+ tree root");
        }
        new_root->page_header_->parent_page = INDEX_NO_PAGE;
        new_root->page_header_->key_count = 0;
        new_root->page_header_->is_leaf = false;
        new_root->page_header_->previous_leaf_page = INDEX_NO_PAGE;
        new_root->page_header_->next_leaf_page = INDEX_NO_PAGE;
        const Rid left_child{.page_no = old_node->get_page_no(), .slot_no = 0};
        const Rid right_child{.page_no = new_node->get_page_no(), .slot_no = 0};
        new_root->insert_pair(0, old_node->get_key(0), left_child);
        new_root->insert_pair(1, key, right_child);

        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());
        file_header_->root_page_ = new_root->get_page_no();
        old_node.mark_dirty();
        new_node.mark_dirty();
        new_root.mark_dirty();
        return;
    }

    IndexNode parent = fetch_node(old_node->get_parent_page_no());
    if (!parent.valid() || parent->is_leaf_page()) {
        throw InternalError("invalid B+ tree parent after split");
    }
    const int child_index = parent->find_child(&old_node.node());
    parent->set_key(child_index, old_node->get_key(0));
    const Rid right_child{.page_no = new_node->get_page_no(), .slot_no = 0};
    parent->insert_pair(child_index + 1, key, right_child);
    old_node.mark_dirty();
    new_node.mark_dirty();
    parent.mark_dirty();

    if (parent->get_size() == parent->get_max_size()) {
        IndexNode new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
    }
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param key 要插入的键。
 * @param value 键对应的 Rid。
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 * @throws DuplicateKeyError 唯一索引中已存在相同 key，此时树保持不变。
 * @pre 调用方不会重复插入完全相同的 (key, value)。
 */
page_id_t BPlusTree::insert_entry(const char* key, const Rid& value, Transaction* transaction) {
    if (key == nullptr) {
        throw InternalError("cannot insert a null B+ tree key");
    }
    std::scoped_lock lock{root_latch_};
    if (is_unique()) {
        std::vector<Rid> existing;
        if (get_value_impl(key, &existing)) {
            throw DuplicateKeyError();
        }
    }

    if (is_empty()) {
        IndexNode root = create_node();
        if (!root.valid()) {
            throw InternalError("buffer pool has no free page for B+ tree root");
        }
        root->page_header_->parent_page = INDEX_NO_PAGE;
        root->page_header_->key_count = 0;
        root->page_header_->is_leaf = true;
        root->page_header_->previous_leaf_page = INDEX_LEAF_HEADER_PAGE;
        root->page_header_->next_leaf_page = INDEX_LEAF_HEADER_PAGE;
        root->insert(key, value);
        root.mark_dirty();

        const page_id_t root_page = root->get_page_no();
        file_header_->root_page_ = root_page;
        file_header_->first_leaf_ = root_page;
        file_header_->last_leaf_ = root_page;
        IndexNode sentinel = fetch_node(INDEX_LEAF_HEADER_PAGE);
        if (!sentinel.valid()) {
            throw InternalError("failed to fetch leaf sentinel");
        }
        sentinel->set_prev_leaf(root_page);
        sentinel->set_next_leaf(root_page);
        sentinel.mark_dirty();
        return root_page;
    }

    auto [leaf, root_latched] = find_leaf_page(key, IndexOperation::Insert, transaction);
    (void)root_latched;
    if (!leaf.valid()) {
        throw InternalError("failed to find insertion leaf");
    }
    const page_id_t leaf_page = leaf->get_page_no();
    leaf->insert(key, value);
    leaf.mark_dirty();
    update_ancestor_keys(leaf);

    if (leaf->get_size() == leaf->get_max_size()) {
        IndexNode new_leaf = split(leaf);
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
    }
    return leaf_page;
}

/**
 * @brief 删除 B+ 树中的一条 (key, rid)。
 * @param key 要删除的key值
 * @param rid 要删除的记录位置；同一 key 的其他记录保留
 * @param transaction 事务指针
 * @return 该 (key, rid) 存在并被删除时返回 true
 */
bool BPlusTree::delete_entry(const char* key, const Rid& rid, Transaction* transaction) {
    if (key == nullptr || is_empty()) {
        return false;
    }
    std::scoped_lock lock{root_latch_};
    auto [leaf, root_latched] = find_leaf_page(key, IndexOperation::Delete, transaction);
    (void)root_latched;
    if (!leaf.valid()) {
        return false;
    }

    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE) {
        IndexNode previous = fetch_node(leaf->get_prev_leaf());
        if (!previous.valid() || previous->get_size() == 0 ||
            compare_index_key(previous->get_key(previous->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) < 0) {
            break;
        }
        leaf = std::move(previous);
    }

    bool removed = false;
    while (leaf.valid()) {
        int pos = leaf->lower_bound(key);
        while (pos < leaf->get_size() &&
               compare_index_key(leaf->get_key(pos), key, file_header_->column_types_, file_header_->column_lengths_) ==
                   0) {
            if (*leaf->get_rid(pos) == rid) {
                leaf->erase_pair(pos);
                leaf.mark_dirty();
                removed = true;
                break;
            }
            ++pos;
        }
        if (removed) {
            break;
        }
        if (leaf->get_size() > 0 &&
            compare_index_key(leaf->get_key(leaf->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) > 0) {
            return false;
        }
        if (leaf->get_next_leaf() == INDEX_LEAF_HEADER_PAGE) {
            return false;
        }
        IndexNode next = fetch_node(leaf->get_next_leaf());
        leaf = std::move(next);
    }
    if (!removed) {
        return false;
    }

    if (leaf->is_root_page()) {
        if (leaf->get_size() == 0) {
            adjust_root(leaf);
        } else {
            update_ancestor_keys(leaf);
        }
        return true;
    }

    if (leaf->get_size() < leaf->get_min_size()) {
        coalesce_or_redistribute(leaf, transaction);
    } else {
        update_ancestor_keys(leaf);
    }
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool BPlusTree::coalesce_or_redistribute(IndexNode& node, Transaction* transaction, bool* root_is_latched) {
    if (root_is_latched != nullptr) {
        *root_is_latched = false;
    }
    if (!node.valid()) {
        return false;
    }
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    if (node->get_size() >= node->get_min_size()) {
        update_ancestor_keys(node);
        return false;
    }

    IndexNode parent = fetch_node(node->get_parent_page_no());
    if (!parent.valid() || parent->is_leaf_page()) {
        throw InternalError("invalid B+ tree parent during deletion");
    }
    const int index = parent->find_child(&node.node());
    const int neighbor_index = index > 0 ? index - 1 : index + 1;
    if (neighbor_index < 0 || neighbor_index >= parent->get_size()) {
        throw InternalError("B+ tree page has no sibling for rebalance");
    }
    IndexNode neighbor = fetch_node(parent->value_at(neighbor_index));
    if (!neighbor.valid() || neighbor->is_leaf_page() != node->is_leaf_page()) {
        throw InternalError("invalid B+ tree sibling during deletion");
    }

    if (node->get_size() + neighbor->get_size() >= 2 * node->get_min_size()) {
        redistribute(neighbor, node, parent, index);
        // If the leftmost child changed its minimum, that value is also the
        // parent's subtree minimum. Propagate it through the remaining ancestors.
        if (!parent->is_root_page()) {
            update_ancestor_keys(parent);
        }
        return false;
    }
    return coalesce(neighbor, node, parent, index, transaction, root_is_latched);
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesce_or_redistribute()
 */
bool BPlusTree::adjust_root(IndexNode& old_root_node) {
    if (!old_root_node.valid()) {
        return false;
    }
    if (old_root_node->is_leaf_page()) {
        if (old_root_node->get_size() != 0) {
            return false;
        }
        const page_id_t old_root_page = old_root_node->get_page_no();
        unlink_leaf(old_root_node);
        file_header_->root_page_ = INDEX_NO_PAGE;
        file_header_->first_leaf_ = INDEX_LEAF_HEADER_PAGE;
        file_header_->last_leaf_ = INDEX_LEAF_HEADER_PAGE;
        record_page_deletion();
        PageGuard old_root_guard = old_root_node.release();
        (void)old_root_guard.drop();
        (void)buffer_pool_manager_->delete_page(PageId{.fd = file_descriptor_, .page_no = old_root_page});
        return true;
    }

    if (old_root_node->get_size() != 1) {
        return false;
    }
    const page_id_t old_root_page = old_root_node->get_page_no();
    const page_id_t child_page = old_root_node->value_at(0);
    IndexNode child = fetch_node(child_page);
    if (!child.valid()) {
        throw InternalError("failed to fetch only child of internal root");
    }
    child->set_parent_page_no(INDEX_NO_PAGE);
    child.mark_dirty();
    file_header_->root_page_ = child_page;
    record_page_deletion();
    PageGuard old_root_guard = old_root_node.release();
    (void)old_root_guard.drop();
    (void)buffer_pool_manager_->delete_page(PageId{.fd = file_descriptor_, .page_no = old_root_page});
    return true;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void BPlusTree::redistribute(IndexNode& neighbor_node, IndexNode& node, IndexNode& parent, int index) {
    if (index > 0) {
        const int source = neighbor_node->get_size() - 1;
        node->insert_pair(0, neighbor_node->get_key(source), *neighbor_node->get_rid(source));
        neighbor_node->erase_pair(source);
        parent->set_key(index, node->get_key(0));
        if (!node->is_leaf_page()) {
            update_child_parent(node, 0);
        }
    } else {
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);
        parent->set_key(index, node->get_key(0));
        if (neighbor_node->get_size() > 0) {
            parent->set_key(index + 1, neighbor_node->get_key(0));
        }
        if (!node->is_leaf_page()) {
            update_child_parent(node, node->get_size() - 1);
        }
    }
    neighbor_node.mark_dirty();
    node.mark_dirty();
    parent.mark_dirty();
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 * Move all the key & value pairs from one page to its sibling page, and notify buffer pool manager to delete this page.
 * Parent page must be adjusted to take info of deletion into account. Remember to deal with coalesce or redistribute
 * recursively if necessary.
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @param transaction 当前事务；不需要并发控制时可为 nullptr。
 * @param root_is_latched 传出根结点的加锁状态。
 * @return true means parent node should be deleted, false means no deletion happend
 * @note Assume that *neighbor_node is the left sibling of *node (neighbor -> node)
 */
bool BPlusTree::coalesce(IndexNode& neighbor_node,
                         IndexNode& node,
                         IndexNode& parent,
                         int index,
                         Transaction* transaction,
                         bool* root_is_latched) {
    IndexNode* left = &neighbor_node;
    IndexNode* right = &node;
    int right_index = index;
    if (index == 0) {
        left = &node;
        right = &neighbor_node;
        right_index = 1;
    }

    const int moved = (*right)->get_size();
    const int old_left_size = (*left)->get_size();
    if (moved > 0) {
        (*left)->insert_pairs(old_left_size, (*right)->get_key(0), (*right)->get_rid(0), moved);
        if (!(*left)->is_leaf_page()) {
            for (int i = 0; i < moved; ++i) {
                update_child_parent(*left, old_left_size + i);
            }
        }
    }
    if ((*left)->is_leaf_page() && (*right)->get_page_no() == file_header_->last_leaf_) {
        file_header_->last_leaf_ = (*left)->get_page_no();
    }
    if ((*left)->is_leaf_page()) {
        unlink_leaf(*right);
    }

    parent->erase_pair(right_index);
    left->mark_dirty();
    right->mark_dirty();
    parent.mark_dirty();
    record_page_deletion();

    // Coalescing may remove a page that supplied the minimum key for its
    // parent. Update that chain before the parent itself can be rebalanced.
    update_ancestor_keys(*left);

    const page_id_t removed_page = (*right)->get_page_no();
    PageGuard removed_guard = right->release();
    (void)removed_guard.drop();
    (void)buffer_pool_manager_->delete_page(PageId{.fd = file_descriptor_, .page_no = removed_page});

    if (parent->is_root_page()) {
        return adjust_root(parent);
    }
    if (parent->get_size() < parent->get_min_size()) {
        return coalesce_or_redistribute(parent, transaction, root_is_latched);
    }
    return false;
}

/**
 * @brief 将索引扫描位置转换为表记录位置 Rid。
 *
 * @param position 叶子页号和页内槽号。
 * @return 对应索引项保存的 Rid。
 * @note IndexPosition 描述索引内部位置，Rid 描述表记录位置，二者含义不同。
 */
Rid BPlusTree::get_rid(const IndexPosition& position) const {
    IndexNode node = fetch_node(position.page_no);
    if (!node.valid() || position.slot_no < 0 || position.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    return *node->get_rid(position.slot_no);
}

/**
 * @brief 返回整棵树中第一个 key >= 目标 key 的索引项位置。
 *
 * @param key 目标 key
 * @return IndexPosition 可作为 IndexScan 的起点
 * @note 位置规范：若结果恰好落在某个非最后叶子的末尾（slot_no == size），应改为下一叶子的
 *       第 0 个槽；若所有 key 都小于目标 key，返回 leaf_end()。这样得到的位置才能与
 *       IndexScan::next() 产生的位置直接比较。
 */
IndexPosition BPlusTree::lower_bound(const char* key) {
    std::scoped_lock lock{root_latch_};
    return lower_bound_impl(key);
}

IndexPosition BPlusTree::lower_bound_impl(const char* key) {
    if (key == nullptr || is_empty()) {
        return leaf_end();
    }
    auto [leaf, root_latched] = find_leaf_page(key, IndexOperation::Find, nullptr);
    (void)root_latched;
    if (!leaf.valid()) {
        return leaf_end();
    }

    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE) {
        IndexNode previous = fetch_node(leaf->get_prev_leaf());
        if (!previous.valid() || previous->get_size() == 0 ||
            compare_index_key(previous->get_key(previous->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) < 0) {
            break;
        }
        leaf = std::move(previous);
    }

    while (leaf.valid()) {
        const int slot = leaf->lower_bound(key);
        if (slot < leaf->get_size()) {
            return {.page_no = leaf->get_page_no(), .slot_no = slot};
        }
        if (leaf->get_next_leaf() == INDEX_LEAF_HEADER_PAGE) {
            break;
        }
        IndexNode next = fetch_node(leaf->get_next_leaf());
        leaf = std::move(next);
    }
    return leaf_end();
}

/**
 * @brief 返回整棵树中第一个 key > 目标 key 的索引项位置。
 *
 * @param key 目标 key
 * @return IndexPosition 可作为 IndexScan 的终点
 * @note 位置规范与 lower_bound() 相同。
 */
IndexPosition BPlusTree::upper_bound(const char* key) {
    std::scoped_lock lock{root_latch_};
    return upper_bound_impl(key);
}

IndexPosition BPlusTree::upper_bound_impl(const char* key) {
    if (key == nullptr || is_empty()) {
        return leaf_end();
    }
    auto [leaf, root_latched] = find_leaf_page(key, IndexOperation::Find, nullptr);
    (void)root_latched;
    if (!leaf.valid()) {
        return leaf_end();
    }

    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE) {
        IndexNode previous = fetch_node(leaf->get_prev_leaf());
        if (!previous.valid() || previous->get_size() == 0 ||
            compare_index_key(previous->get_key(previous->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) <= 0) {
            break;
        }
        leaf = std::move(previous);
    }

    while (leaf.valid()) {
        const int slot = leaf->upper_bound(key);
        if (slot < leaf->get_size()) {
            return {.page_no = leaf->get_page_no(), .slot_no = slot};
        }
        if (leaf->get_next_leaf() == INDEX_LEAF_HEADER_PAGE) {
            break;
        }
        IndexNode next = fetch_node(leaf->get_next_leaf());
        leaf = std::move(next);
    }
    return leaf_end();
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IndexScan的最后一个
 *
 * @return IndexPosition
 */
IndexPosition BPlusTree::leaf_end() const {
    IndexNode node = fetch_node(file_header_->last_leaf_);
    if (!node.valid()) {
        throw InternalError("failed to fetch last leaf for index scan");
    }
    return {.page_no = file_header_->last_leaf_, .slot_no = node->get_size()};
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IndexScan的第一个
 *
 * @return IndexPosition
 */
IndexPosition BPlusTree::leaf_begin() const { return {.page_no = file_header_->first_leaf_, .slot_no = 0}; }

IndexNode BPlusTree::fetch_node(page_id_t page_no) const {
    PageGuard guard = buffer_pool_manager_->fetch_page_guard(PageId{.fd = file_descriptor_, .page_no = page_no});
    return {file_header_.get(), std::move(guard)};
}

IndexNode BPlusTree::create_node() {
    PageId new_page_id = {.fd = file_descriptor_, .page_no = INVALID_PAGE_ID};
    PageGuard guard = buffer_pool_manager_->new_page_guard(&new_page_id);
    if (!guard) {
        return {};
    }
    file_header_->page_count_++;
    return {file_header_.get(), std::move(guard)};
}

void BPlusTree::update_ancestor_keys(IndexNode& node) {
    page_id_t current_page = node->get_page_no();
    while (true) {
        IndexNode current = fetch_node(current_page);
        if (!current.valid() || current->get_parent_page_no() == INDEX_NO_PAGE) {
            break;
        }
        IndexNode parent = fetch_node(current->get_parent_page_no());
        if (!parent.valid()) {
            break;
        }
        const int rank = parent->find_child(&current.node());
        if (memcmp(parent->get_key(rank), current->get_key(0), file_header_->key_length_) == 0) {
            break;
        }
        parent->set_key(rank, current->get_key(0));
        parent.mark_dirty();
        current_page = parent->get_page_no();
    }
}

// 框架辅助：调用即可。这段失败回滚是框架内部实现，作业不要模仿。
void BPlusTree::unlink_leaf(IndexNode& leaf) {
    if (!leaf.valid() || !leaf->is_leaf_page()) {
        throw InternalError("unlink_leaf requires a valid leaf node");
    }

    const page_id_t leaf_page = leaf->get_page_no();
    const page_id_t prev_page = leaf->get_prev_leaf();
    const page_id_t next_page = leaf->get_next_leaf();

    {
        IndexNode prev = fetch_node(prev_page);
        if (!prev.valid()) {
            throw InternalError("unlink_leaf failed to fetch previous leaf");
        }
        prev->set_next_leaf(next_page);
        prev.mark_dirty();
    }
    {
        IndexNode next = fetch_node(next_page);
        if (!next.valid()) {
            IndexNode prev = fetch_node(prev_page);
            if (prev.valid()) {
                prev->set_next_leaf(leaf_page);
                prev.mark_dirty();
            }
            throw InternalError("unlink_leaf failed to fetch next leaf; previous link rolled back");
        }
        next->set_prev_leaf(prev_page);
        next.mark_dirty();
    }
}

void BPlusTree::record_page_deletion() { file_header_->page_count_--; }

void BPlusTree::update_child_parent(IndexNode& node, int child_idx) {
    if (!node.valid() || node->is_leaf_page()) {
        return;
    }
    IndexNode child = fetch_node(node->value_at(child_idx));
    if (!child.valid()) {
        return;
    }
    child->set_parent_page_no(node->get_page_no());
    child.mark_dirty();
}
