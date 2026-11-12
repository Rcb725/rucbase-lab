// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "rm_file_handle.h"

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    (void)context;
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.page_no >= file_hdr_.num_pages || rid.slot_no < 0 ||
        rid.slot_no >= file_hdr_.num_records_per_page) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    RmPageGuard page = fetch_page_guard(rid.page_no);
    if (!page.valid()) {
        throw InternalError("failed to fetch record page");
    }
    if (!Bitmap::is_set(page->bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    return std::make_unique<RmRecord>(file_hdr_.record_size, page->get_slot(rid.slot_no));
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {const char*} buf 要插入的记录数据，只读取 record_size 字节
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(const char* buf, Context* context) {
    (void)context;
    if (buf == nullptr) {
        throw InternalError("cannot insert a record from null data");
    }

    RmPageHandle page = create_page_handle();
    const int slot_no = Bitmap::first_bit(false, page.bitmap, file_hdr_.num_records_per_page);
    if (slot_no >= file_hdr_.num_records_per_page) {
        buffer_pool_manager_->unpin_page(page.page->get_page_id(), false);
        throw InternalError("free page has no free record slot");
    }

    memcpy(page.get_slot(slot_no), buf, static_cast<size_t>(file_hdr_.record_size));
    Bitmap::set(page.bitmap, slot_no);
    page.page_hdr->num_records++;
    const Rid rid{.page_no = page.page->get_page_id().page_no, .slot_no = slot_no};

    if (page.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page.page_hdr->next_free_page_no;
        page.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    buffer_pool_manager_->unpin_page(page.page->get_page_id(), true);
    return rid;
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {const char*} buf 要插入的记录数据，只读取 record_size 字节
 */
void RmFileHandle::insert_record(const Rid& rid, const char* buf) {
    // 后续恢复/回滚实验使用的扩展接口，不属于 Lab1 评分任务。
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    (void)context;
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.page_no >= file_hdr_.num_pages || rid.slot_no < 0 ||
        rid.slot_no >= file_hdr_.num_records_per_page) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    RmPageGuard page = fetch_page_guard(rid.page_no);
    if (!page.valid()) {
        throw InternalError("failed to fetch record page");
    }
    RmPageHandle& handle = page.handle();
    if (!Bitmap::is_set(handle.bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    const bool was_full = handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::reset(handle.bitmap, rid.slot_no);
    handle.page_hdr->num_records--;
    if (was_full) {
        release_page_handle(handle);
    }
    page.mark_dirty();
}

/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {const char*} buf 新记录的数据，只读取 record_size 字节
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, const char* buf, Context* context) {
    (void)context;
    if (buf == nullptr) {
        throw InternalError("cannot update a record from null data");
    }
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.page_no >= file_hdr_.num_pages || rid.slot_no < 0 ||
        rid.slot_no >= file_hdr_.num_records_per_page) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    RmPageGuard page = fetch_page_guard(rid.page_no);
    if (!page.valid()) {
        throw InternalError("failed to fetch record page");
    }
    if (!Bitmap::is_set(page->bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    memcpy(page->get_slot(rid.slot_no), buf, static_cast<size_t>(file_hdr_.record_size));
    page.mark_dirty();
}

/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
 */
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    if (page_no < RM_FIRST_RECORD_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError("record file", page_no);
    }
    Page* page = buffer_pool_manager_->fetch_page(PageId{.fd = fd_, .page_no = page_no});
    if (page == nullptr) {
        throw InternalError("buffer pool has no available frame");
    }
    return {&file_hdr_, page};
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    PageId page_id{.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page* page = buffer_pool_manager_->new_page(&page_id);
    if (page == nullptr) {
        throw InternalError("buffer pool has no available frame");
    }

    RmPageHandle handle(&file_hdr_, page);
    handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    handle.page_hdr->num_records = 0;
    Bitmap::init(handle.bitmap, file_hdr_.bitmap_size);
    file_hdr_.num_pages = page_id.page_no + 1;
    file_hdr_.first_free_page_no = page_id.page_no;
    return handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_handle();
    }
    return fetch_page_handle(file_hdr_.first_free_page_no);
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle& page_handle) {
    const page_id_t page_no = page_handle.page->get_page_id().page_no;
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_no;
}
