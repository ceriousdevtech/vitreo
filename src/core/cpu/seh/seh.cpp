#include "core/cpu/seh/seh.hpp"

#include <algorithm>

namespace vitreo::cpu {

bool SehManager::add_function_table(const RuntimeFunction* table, u32 entry_count,
                                     GuestAddr base_address) {
    if (!table || entry_count == 0) return false;
    
    std::lock_guard lock(m_mutex);
    
    FunctionTableEntry entry;
    entry.base_address = base_address;
    entry.functions.assign(table, table + entry_count);
    
    // Sort by begin_address for binary search
    std::sort(entry.functions.begin(), entry.functions.end(),
              [](const RuntimeFunction& a, const RuntimeFunction& b) {
                  return a.begin_address < b.begin_address;
              });
    
    m_tables.push_back(std::move(entry));
    return true;
}

bool SehManager::delete_function_table(GuestAddr base_address) {
    std::lock_guard lock(m_mutex);
    
    auto it = std::remove_if(m_tables.begin(), m_tables.end(),
                              [base_address](const FunctionTableEntry& e) {
                                  return e.base_address == base_address;
                              });
    if (it == m_tables.end()) return false;
    m_tables.erase(it, m_tables.end());
    return true;
}

const RuntimeFunction* SehManager::lookup_function_entry(
    GuestAddr control_pc, GuestAddr& image_base_out) const {
    std::lock_guard lock(m_mutex);
    
    for (const auto& table : m_tables) {
        // Convert control_pc to RVA relative to this table's base
        if (control_pc < table.base_address) continue;
        u32 rva = static_cast<u32>(control_pc - table.base_address);
        
        // Binary search for function containing this RVA
        auto it = std::upper_bound(
            table.functions.begin(), table.functions.end(), rva,
            [](u32 addr, const RuntimeFunction& f) { return addr < f.begin_address; });
        
        if (it != table.functions.begin()) {
            --it;
            if (it->contains(rva)) {
                image_base_out = table.base_address;
                return &(*it);
            }
        }
    }
    
    return nullptr;
}

i32 SehManager::dispatch_exception(const ExceptionRecord& record) {
    // Phase 1: Look up handler for faulting RIP
    GuestAddr image_base = 0;
    const auto* func = lookup_function_entry(record.exception_address, image_base);
    
    // If no function table entry, fall through to unhandled filter
    if (!func) {
        if (m_unhandled_filter) {
            return m_unhandled_filter(record);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    
    // For now, handler dispatch is simplified:
    // In a full implementation, we'd read UnwindInfo from guest memory
    // and call the handler. For initial implementation, just check if
    // there's an unhandled filter.
    if (m_unhandled_filter) {
        return m_unhandled_filter(record);
    }
    
    return EXCEPTION_CONTINUE_SEARCH;
}

void SehManager::raise_exception(u32 code, u32 flags, u32 num_args,
                                   const u64* args, GuestAddr rip) {
    ExceptionRecord record;
    record.exception_code = code;
    record.exception_flags = flags;
    record.exception_address = rip;
    record.number_parameters = std::min(num_args, u32(15));
    if (args && record.number_parameters > 0) {
        for (u32 i = 0; i < record.number_parameters; ++i) {
            record.exception_information[i] = args[i];
        }
    }
    
    dispatch_exception(record);
}

UnhandledExceptionFilter SehManager::set_unhandled_exception_filter(
    UnhandledExceptionFilter filter) {
    std::lock_guard lock(m_mutex);
    auto prev = std::move(m_unhandled_filter);
    m_unhandled_filter = std::move(filter);
    return prev;
}

usize SehManager::function_count() const {
    std::lock_guard lock(m_mutex);
    usize count = 0;
    for (const auto& t : m_tables) {
        count += t.functions.size();
    }
    return count;
}

} // namespace vitreo::cpu
