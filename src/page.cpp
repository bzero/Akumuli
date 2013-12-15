/*
 * Copyright (c) 2013 Eugene Lazin <4lazin@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#include <cstring>
#include <cassert>
#include <algorithm>
#include <apr_time.h>
#include "timsort.hpp"
#include "page.h"
#include "akumuli_def.h"


namespace Akumuli {

//---------------Timestamp

TimeStamp TimeStamp::utc_now() noexcept {
    int64_t t = apr_time_now();
    return { t };
}

bool TimeStamp::operator  < (TimeStamp other) const noexcept {
    return precise < other.precise;
}

bool TimeStamp::operator  > (TimeStamp other) const noexcept {
    return precise > other.precise;
}

bool TimeStamp::operator == (TimeStamp other) const noexcept {
    return precise == other.precise;
}

bool TimeStamp::operator <= (TimeStamp other) const noexcept {
    return precise <= other.precise;
}

bool TimeStamp::operator >= (TimeStamp other) const noexcept {
    return precise >= other.precise;
}

//------------------------

String::String(UnsafeTag, const char* str)
{
    size = space_needed(str);
    // Things can go wrong here
    strcpy(string, str);
}

int32_t String::space_needed(const char* str) {
    return strlen(str) + sizeof(int32_t) + 1;
}

int32_t String::write(void* dest, size_t dest_cap, const char* str) {
    int32_t len = String::space_needed(str);
    if (len < dest_cap) {
        return -len;
    }
    auto str_ptr = reinterpret_cast<String*>(dest);
    str_ptr->size = len;
    strcpy(str_ptr->string, str);
    return 0;
}

size_t MetadataRecord::space_needed(const char* str) noexcept {
    return String::space_needed(str) + sizeof(TypeTag);
}

MetadataRecord::MetadataRecord(TimeStamp time)
    : tag(TypeTag::DATE_TIME)
    , time(time)
{
}

MetadataRecord::MetadataRecord(uint64_t value)
    : tag(TypeTag::INTEGER)
    , integer(value)
{
}

MetadataRecord::MetadataRecord(UnsafeTag, const char* str)
    : tag(TypeTag::STRING)
{
}

Entry::Entry(uint32_t length)
    : length(length)
    , time {}
    , param_id {}
{
}

Entry::Entry(uint32_t param_id, TimeStamp timestamp, uint32_t length)
    : param_id(param_id)
    , time(timestamp)
    , length(length)
{
}

uint32_t Entry::get_size(uint32_t load_size) noexcept {
    return sizeof(Entry) - sizeof(uint32_t) + load_size;
}

aku_MemRange Entry::get_storage() const noexcept {
    return { (void*)value, length };
}


Entry2::Entry2(uint32_t param_id, TimeStamp time, aku_MemRange range)
    : param_id(param_id)
    , time(time)
    , range(range)
{
}


PageBoundingBox::PageBoundingBox()
    : max_id(0)
    , min_id(std::numeric_limits<uint32_t>::max())
{
    max_timestamp.precise = 0;
    min_timestamp.precise = std::numeric_limits<uint64_t>::max();
}


const char* PageHeader::cdata() const noexcept {
    return reinterpret_cast<const char*>(this);
}

char* PageHeader::data() noexcept {
    return reinterpret_cast<char*>(this);
}

PageHeader::PageHeader(PageType type, uint32_t count, uint64_t length, uint32_t page_id)
    : type(type)
    , count(count)
    , last_offset(length)
    , length(length)
    , overwrites_count(0)
    , page_id(page_id)
    , bbox()
{
}

int PageHeader::get_entries_count() const noexcept {
    return (int)this->count;
}

int PageHeader::get_free_space() const noexcept {
    auto begin = reinterpret_cast<const char*>(page_index + count);
    const char* end = 0;
    end = cdata() + last_offset;
    return end - begin;
}

void PageHeader::update_bounding_box(ParamId param, TimeStamp time) noexcept {
    if (param > bbox.max_id) {
        bbox.max_id = param;
    } else if (param < bbox.min_id) {
        bbox.min_id = param;
    }
    if (time > bbox.max_timestamp) {
        bbox.max_timestamp = time;
    } else if (time < bbox.min_timestamp) {
        bbox.min_timestamp = time;
    }
}

bool PageHeader::inside_bbox(ParamId param, TimeStamp time) const noexcept {
    return time  <= bbox.max_timestamp
        && time  >= bbox.min_timestamp
        && param <= bbox.max_id
        && param >= bbox.min_id;
}

void PageHeader::clear() noexcept {
    count = 0;
    overwrites_count++;
    last_offset = length;
    bbox = PageBoundingBox();
}

int PageHeader::add_entry(Entry const& entry) noexcept {
    auto space_required = entry.length + sizeof(EntryOffset);
    if (entry.length < sizeof(Entry)) {
        return AKU_WRITE_STATUS_BAD_DATA;
    }
    if (space_required > get_free_space()) {
        return AKU_WRITE_STATUS_OVERFLOW;
    }
    char* free_slot = data() + last_offset;
    free_slot -= entry.length;
    memcpy((void*)free_slot, (void*)&entry, entry.length);
    last_offset = free_slot - cdata();
    page_index[count] = last_offset;
    count++;
    update_bounding_box(entry.param_id, entry.time);
    return AKU_WRITE_STATUS_SUCCESS;
}

int PageHeader::add_entry(Entry2 const& entry) noexcept {
    auto space_required = entry.range.length + sizeof(Entry2) + sizeof(EntryOffset);
    if (space_required > get_free_space()) {
        return AKU_WRITE_STATUS_OVERFLOW;
    }
    char* free_slot = 0;
    free_slot = data() + last_offset;
    // FIXME: reorder to improve memory performance
    // Write data
    free_slot -= entry.range.length;
    memcpy((void*)free_slot, entry.range.address, entry.range.length);
    // Write length
    free_slot -= sizeof(uint32_t);
    *(uint32_t*)free_slot = entry.range.length;
    // Write paramId and timestamp
    free_slot -= sizeof(Entry2);
    memcpy((void*)free_slot, (void*)&entry, sizeof(Entry2));
    last_offset = free_slot - cdata();
    page_index[count] = last_offset;
    count++;
    update_bounding_box(entry.param_id, entry.time);
    return AKU_WRITE_STATUS_SUCCESS;
}

const Entry* PageHeader::find_entry(int index) const noexcept {
    if (index >= 0 && index < count) {
        auto offset = page_index[index];
        auto ptr = cdata() + offset;
        auto entry_ptr = reinterpret_cast<const Entry*>(ptr);
        return entry_ptr;
    }
    return 0;
}

int PageHeader::get_entry_length(int entry_index) const noexcept {
    auto entry_ptr = find_entry(entry_index);
    if (entry_ptr) {
        return entry_ptr->length;
    }
    return 0;
}

int PageHeader::copy_entry(int index, Entry* receiver) const noexcept {
    auto entry_ptr = find_entry(index);
    if (entry_ptr) {
        if (entry_ptr->length > receiver->length) {
            return -1*entry_ptr->length;
        }
        memcpy((void*)receiver, (void*)entry_ptr, entry_ptr->length);
        return entry_ptr->length;
    }
    return 0;
}

template<class RandomIt, class Cmp>
void ins_sort(RandomIt start, RandomIt end, Cmp cmp) {
    for (RandomIt k = start; k < end; ++k) {
        RandomIt l = k;
        RandomIt l_prev = l - 1;
        while (l_prev >= start && cmp(*l_prev, *l)) {
            std::iter_swap(l, l_prev);
            l--;
            l_prev--;
        }
    }
}

void PageHeader::sort() noexcept {
    auto begin = page_index;
    auto end = page_index + count;
    gfx::timsort(begin, end, [&](EntryOffset a, EntryOffset b) {
        auto ea = reinterpret_cast<const Entry*>(cdata() + a);
        auto eb = reinterpret_cast<const Entry*>(cdata() + b);
        auto ta = std::tuple<uint64_t, uint32_t>(ea->time.precise, ea->param_id);
        auto tb = std::tuple<uint64_t, uint32_t>(eb->time.precise, eb->param_id);
        return ta < tb;
    });
}

bool PageHeader::binary_search
    ( ParamId param
    , TimeStamp time_lowerbound
    , EntryOffset* out_offset
    ) const noexcept
{
    auto begin = page_index;
    auto end = page_index + count;
    auto it_res = std::lower_bound(begin, end, std::make_tuple(time_lowerbound.precise, param),
        [&](EntryOffset a, std::tuple<uint64_t, uint32_t> b) {
            auto ea = reinterpret_cast<const Entry*>(cdata() + a);
            auto ta = std::tuple<uint64_t, uint32_t>(ea->time.precise, ea->param_id);
            return ta < b;
    });
    if (it_res != end) {
        *out_offset = *it_res;
        return true;
    }
    return false;
}

}  // namepsace