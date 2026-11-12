// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "rm_scan.h"

#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle* file_handle) : file_handle_(file_handle) {
    rid_ = Rid{.page_no = RM_FIRST_RECORD_PAGE, .slot_no = -1};
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    if (is_end()) {
        return;
    }

    int page_no = rid_.page_no;
    int slot_no = rid_.slot_no;
    const RmFileHdr file_hdr = file_handle_->file_hdr_;
    while (page_no < file_hdr.num_pages) {
        if (page_no < RM_FIRST_RECORD_PAGE) {
            page_no = RM_FIRST_RECORD_PAGE;
            slot_no = -1;
            continue;
        }

        RmPageHandle page = file_handle_->fetch_page_handle(page_no);
        const int next_slot = Bitmap::next_bit(true, page.bitmap, file_hdr.num_records_per_page, slot_no);
        file_handle_->buffer_pool_manager_->unpin_page(page.page->get_page_id(), false);
        if (next_slot < file_hdr.num_records_per_page) {
            rid_ = Rid{.page_no = page_no, .slot_no = next_slot};
            return;
        }

        page_no++;
        slot_no = -1;
    }
    rid_ = Rid{.page_no = RM_NO_PAGE, .slot_no = -1};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    return rid_.page_no == RM_NO_PAGE || rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const { return rid_; }
