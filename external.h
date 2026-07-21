#ifndef EXTERNAL_H
#define EXTERNAL_H

#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <sys/uio.h>
#include <unistd.h>
#include <dirent.h>

namespace ExternalLib {
// ── Memory I/O ───────────────────────────────────────────────────────────────

inline bool read_mem(pid_t pid, uintptr_t remote_addr, void* local_buffer, size_t size) {
    iovec local  = {local_buffer,                          size};
    iovec remote = {reinterpret_cast<void*>(remote_addr), size};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

inline bool write_mem(pid_t pid, uintptr_t remote_addr, const void* local_buffer, size_t size) {
    iovec local  = {const_cast<void*>(local_buffer),      size};
    iovec remote = {reinterpret_cast<void*>(remote_addr), size};
    return process_vm_writev(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

template<typename T>
inline T read_val(pid_t pid, uintptr_t addr) {
    T val{};
    read_mem(pid, addr, &val, sizeof(T));
    return val;
}

template<typename T>
inline void write_val(pid_t pid, uintptr_t addr, T val) {
    write_mem(pid, addr, &val, sizeof(T));
}

// ── Roblox helpers ───────────────────────────────────────────────────────────

inline std::string read_name(pid_t pid, uintptr_t addr, size_t max_len = 256) {
    uint8_t fmt = 0;
    if (!read_mem(pid, addr, &fmt, 1)) return "";

    size_t length = fmt >> 1;
    if (length == 0) return "";

    size_t read_len = std::min(length, max_len);
    std::string buf(read_len, '\0');

    if (fmt & 1) {
        uint64_t ptr = read_val<uint64_t>(pid, addr) & ~0xFFULL;
        if (ptr < 0x10000) return "";
        read_mem(pid, ptr, buf.data(), read_len);
    } else {
        read_mem(pid, addr + 1, buf.data(), read_len);
    }

    return buf;
}

inline std::string get_class(pid_t pid, uintptr_t inst) {
    uint64_t cd       = read_val<uint64_t>(pid, inst + 0x18); if (!cd)       return "";
    uint64_t name_ptr = read_val<uint64_t>(pid, cd   + 0x8);  if (!name_ptr) return "";
    return read_name(pid, name_ptr);
}

inline std::vector<uint64_t> get_children(pid_t pid, uintptr_t inst) {
    std::vector<uint64_t> children;

    uint64_t ctl   = read_val<uint64_t>(pid, inst + 0x78); if (!ctl) return children;
    uint64_t first = read_val<uint64_t>(pid, ctl);
    uint64_t end   = read_val<uint64_t>(pid, ctl + 0x8);
    if (!first || !end) return children;

    for (uint64_t cur = first; cur != end; cur += 0x10) {
        if (uint64_t child = read_val<uint64_t>(pid, cur))
            children.push_back(child);
    }
    return children;
}

inline uint64_t find_child(pid_t pid, uintptr_t parent, const std::string& name) {
    for (uint64_t c : get_children(pid, parent))
        if (get_class(pid, c) == name) return c;
    return 0;
}

// ── Process utilities ─────────────────────────────────────────────────────────

inline uintptr_t find_base(pid_t pid) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    if (!maps.is_open()) return 0;

    for (std::string line; std::getline(maps, line);) {
        std::string addr_range, perms, offset, dev, inode, pathname;
        std::istringstream ss(line);
        ss >> addr_range >> perms >> offset >> dev >> inode;
        if (!(ss >> pathname)) continue;

        if (perms.find('x') == std::string::npos) continue;
        if (offset   != "00000000")               continue;
        if (line.find("/memfd:") == std::string::npos) continue;

        size_t dash = addr_range.find('-');
        if (dash == std::string::npos) continue;
        return std::stoull(addr_range.substr(0, dash), nullptr, 16);
    }
    return 0;
}

inline pid_t get_pid_by_name(const std::string& process_name) {
    DIR* dir = opendir("/proc");
    if (!dir) return 0;

    pid_t result = 0;
    while (dirent* entry = readdir(dir)) {
        int pid = atoi(entry->d_name);
        if (pid <= 0) continue;

        std::ifstream comm("/proc/" + std::string(entry->d_name) + "/comm");
        std::string name;
        if (comm.is_open() && std::getline(comm, name) && name == process_name) {
            result = pid;
            break;
        }
    }
    closedir(dir);
    return result;
}

}

#endif // EXTERNAL_H
