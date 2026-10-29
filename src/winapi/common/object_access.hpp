#pragma once
// Shared object-manager authorization model. No guest pointers or host identities.
#include <vitreo/common.hpp>
#include <span>
#include <vector>
#include <optional>
#include <cstring>
#include "winapi/common/object_access_constants.hpp"
namespace vitreo::winapi::object_access {
struct Sid { std::vector<u8> bytes; u32 attributes{}; };
struct Token {
    std::vector<u8> user;
    bool user_deny_only{};
    std::vector<Sid> groups;
    std::vector<Sid> restricting;
    std::vector<u8> default_dacl;
    bool security_privilege{};
    bool take_ownership_privilege{};
    /// Mandatory integrity level RID (S-1-16-RID): 0x2000 medium, 0x3000 high.
    u32 integrity_rid{0x2000};
    /// SeSecurityPrivilege is in the token at all (enabled or not).
    bool security_privilege_held{};
};
struct Descriptor {
    std::vector<u8> owner;
    // nullopt means no DACL (unrestricted), an 8-byte zero-ACE ACL denies access.
    std::optional<std::vector<u8>> dacl;
    /// A SACL ACE this model cannot represent (not a mandatory label, audit or
    /// alarm ACE).
    bool unsupported_sacl{};
    /// SYSTEM_MANDATORY_LABEL_ACE from the SACL: the object's integrity level
    /// and its NO_WRITE_UP/NO_READ_UP/NO_EXECUTE_UP policy.
    bool has_label{};
    u32 label_rid{};
    u32 label_policy{};
    /// The SACL holds SYSTEM_AUDIT/ALARM ACEs (no effect on access).
    bool has_audit{};
};
enum class Result { Granted, Denied, Unsupported, Invalid };
inline u32 word(std::span<const u8> b, usize at) {
    u32 n=0; if(at+4<=b.size()) std::memcpy(&n,b.data()+at,4); return n;
}
inline bool valid_sid(std::span<const u8> sid) {
    return sid.size()>=8 && sid[0]==1 && sid[1]<=15 && sid.size()==8+4u*sid[1];
}
/// Offset of the SID inside an ACE of `type`, 0 for a type that carries none
/// RtlValidAcl examines (unknown types), or -1 for an object ACE that is too
/// short to hold its flags. Plain ACEs are Mask + SID; object ACEs put Flags
/// and the GUIDs those flags select before the SID.
inline i32 ace_sid_offset(std::span<const u8> ace,u32 ace_size) {
    switch(ace[0]){
        case 0x00: case 0x01: case 0x02: case 0x03:  // allowed/denied/audit/alarm
        case 0x09: case 0x0A: case 0x0D: case 0x0E:  // callback forms
        case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
            return 8;
        case 0x05: case 0x06: case 0x07: case 0x08:  // object forms
        case 0x0B: case 0x0C: case 0x0F: case 0x10: {
            if(ace_size<12)return -1;
            const u32 flags=word(ace,8);
            return static_cast<i32>(12+((flags&1u)?16u:0u)+((flags&2u)?16u:0u));
        }
        default:
            return 0;
    }
}
/// RtlValidAcl as measured (tools/oracle_probe/advapi_sdapi_probe ivacl_*,
/// sync_dacl_ace_layout_probe): revision 2..4, AclSize >= 8 and inside the
/// buffer (need not be a multiple of 4), each ACE's AceSize a multiple of 4
/// inside AclSize, a whole SID inside every ACE type that carries one (bytes
/// after the SID are allowed and ignored), object ACEs only in an
/// ACL_REVISION_DS ACL. Sbz1/Sbz2 and unknown ACE types are not examined.
/// `used` receives the bytes the ACL header and its ACEs occupy.
inline bool valid_acl(std::span<const u8> acl,u32* used=nullptr) {
    if(acl.size()<8)return false;
    const u8 revision=acl[0];
    if(revision<2 || revision>4)return false;
    const u32 size=static_cast<u32>(acl[2]|(acl[3]<<8));
    const u32 count=static_cast<u32>(acl[4]|(acl[5]<<8));
    if(size<8 || size>acl.size())return false;
    u32 offset=8;
    for(u32 i=0;i<count;++i){
        if(offset+4>size)return false;
        const u32 ace_size=static_cast<u32>(acl[offset+2]|(acl[offset+3]<<8));
        if(ace_size<4 || (ace_size&3u)!=0 || offset+ace_size>size)return false;
        const auto ace=acl.subspan(offset,ace_size);
        const i32 sid_at=ace_sid_offset(ace,ace_size);
        if(sid_at<0 || (sid_at>8 && revision<4))return false;
        if(sid_at>0){
            const u32 at=static_cast<u32>(sid_at);
            if(at+8>ace_size)return false;
            if(ace[at]!=1 || ace[at+1]>15 || at+8u+4u*ace[at+1]>ace_size)return false;
        }
        offset+=ace_size;
    }
    if(used)*used=offset;
    return true;
}
inline u32 map_generic(u32 mask,u32 all,u32 read,u32 write,u32 execute) {
    u32 out=mask & ~(sync_access::GENERIC_READ|sync_access::GENERIC_WRITE|sync_access::GENERIC_EXECUTE|sync_access::GENERIC_ALL);
    if(mask&sync_access::GENERIC_READ)out|=read;
    if(mask&sync_access::GENERIC_WRITE)out|=write;
    if(mask&sync_access::GENERIC_EXECUTE)out|=execute;
    if(mask&sync_access::GENERIC_ALL)out|=all;
    return out;
}
/// The granted access a file-object handle (file, pipe, mailslot, console)
/// reports through NtQueryObject and FileAccessInformation: GENERIC_* bits
/// mapped through FILE_GENERIC_* (winnt.h; file_access_info_probe), and an
/// unrestricted handle as FILE_ALL_ACCESS. @p unrestricted is the handle
/// table's "no restriction recorded" value.
inline u32 file_granted_access(u32 stored, u32 unrestricted) {
    constexpr u32 kFileAllAccess = 0x001F01FFu;
    if (stored == unrestricted) return kFileAllAccess;
    return map_generic(stored, kFileAllAccess, 0x00120089u /*FILE_GENERIC_READ*/,
                       0x00120116u /*FILE_GENERIC_WRITE*/,
                       0x001200A0u /*FILE_GENERIC_EXECUTE*/);
}
inline Result evaluate(const Descriptor& sd,const Token& token,u32 desired,
                       u32 all,u32 read,u32 write,u32 execute,u32& granted) {
    granted=0;
    if(sd.unsupported_sacl) return Result::Unsupported;
    // A caller below the object's integrity level is restricted by the label
    // policy. The exact rights each policy bit removes are not measured yet
    // (tools/oracle_probe/sync_sacl_label_probe.c only covers callers at or
    // above the label), so that case fails loudly rather than guessing.
    if(sd.has_label && sd.label_policy!=0 && token.integrity_rid<sd.label_rid) return Result::Unsupported;
    const bool maximum=(desired&sync_access::MAXIMUM_ALLOWED)!=0;
    u32 request=map_generic(desired&~sync_access::MAXIMUM_ALLOWED,all,read,write,execute);
    if(request&sync_access::ACCESS_SYSTEM_SECURITY) {
        if(!token.security_privilege)return Result::Denied;
        granted|=sync_access::ACCESS_SYSTEM_SECURITY; request&=~sync_access::ACCESS_SYSTEM_SECURITY;
    }
    u32 owner_rights=0;
    bool explicit_owner=false;
    if(!sd.dacl) {
        if(sd.owner==token.user) owner_rights=sync_access::READ_CONTROL|sync_access::WRITE_DAC;
        if(token.take_ownership_privilege) owner_rights|=sync_access::WRITE_OWNER;
        granted|=request|(maximum?all:0);
        return Result::Granted;
    }
    auto acl=std::span<const u8>(*sd.dacl);
    if(!valid_acl(acl))return Result::Invalid;
    const usize count=static_cast<usize>(acl[4])|(static_cast<usize>(acl[5])<<8);
    struct Ace {bool deny;u32 mask;std::vector<u8> sid;}; std::vector<Ace> aces;
    // An Owner Rights ACE (S-1-3-4) replaces the owner's implicit
    // READ_CONTROL|WRITE_DAC. It matches the object's owner, not a token group.
    usize offset=8;
    for(usize i=0;i<count;++i){
        const usize size=static_cast<usize>(acl[offset+2])|(static_cast<usize>(acl[offset+3])<<8);
        if(!(acl[offset+1]&sync_access::INHERIT_ONLY_ACE)) {
            if(acl[offset]!=sync_access::ACCESS_ALLOWED_ACE_TYPE && acl[offset]!=sync_access::ACCESS_DENIED_ACE_TYPE)return Result::Unsupported;
            // valid_acl proved the whole SID fits; an AceSize larger than
            // Mask + SID leaves bytes after it that Windows ignores
            // (sync_dacl_ace_layout_probe slack4/deny_slack4).
            std::span<const u8> sid=acl.subspan(offset+8,8+4u*acl[offset+9]);
            const bool owner_rights=sid.size()==12 && sid[7]==3 && word(sid,8)==4;
            if(owner_rights) explicit_owner=true;
            aces.push_back({acl[offset]==sync_access::ACCESS_DENIED_ACE_TYPE,
                map_generic(word(acl,offset+4),all,read,write,execute),
                owner_rights?sd.owner:std::vector<u8>(sid.begin(),sid.end())});
        }
        offset+=size;
    }
    if(!explicit_owner && sd.owner==token.user) owner_rights=sync_access::READ_CONTROL|sync_access::WRITE_DAC;
    if(token.take_ownership_privilege) owner_rights|=sync_access::WRITE_OWNER;
    auto pass=[&](bool restricted)->u32 {
        u32 allowed=restricted?0:owner_rights;
        u32 denied=0;
        for(const auto& ace:aces){
            bool matches=!restricted && ace.sid==token.user && (!token.user_deny_only || ace.deny);
            const auto& groups=restricted?token.restricting:token.groups;
            for(const auto& group:groups){
                if(group.bytes==ace.sid && ((group.attributes&sync_access::SE_GROUP_ENABLED) ||
                    (ace.deny && (group.attributes&sync_access::SE_GROUP_USE_FOR_DENY_ONLY))))matches=true;
            }
            if(!matches)continue;
            if(ace.deny)denied|=ace.mask&~allowed;
            else allowed|=ace.mask&~denied;
        }
        return allowed;
    };
    u32 allowed=pass(false);
    if(!token.restricting.empty())allowed&=pass(true);
    if(request&~allowed)return Result::Denied;
    granted|=request|(maximum?(allowed&all):0);
    return Result::Granted;
}
} // namespace vitreo::winapi::object_access
