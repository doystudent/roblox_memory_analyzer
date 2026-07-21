#include <cstdint>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <sys/uio.h>
#include <unistd.h>
#include <dirent.h>

template <typename T>
T Check (T val, const char* msg) { if (!val) throw std::runtime_error(msg); return val; }

// ── Memory I/O ───────────────────────────────────────────────────────────────

bool readMem (pid_t pid, uintptr_t remote_addr, void* local_buffer, size_t size) {
    iovec local  = {local_buffer, size};
    iovec remote = {reinterpret_cast<void*>(remote_addr), size};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

bool writeMem (pid_t pid, uintptr_t remote_addr, const void* local_buffer, size_t size) {
    iovec local  = {const_cast<void*>(local_buffer),      size};
    iovec remote = {reinterpret_cast<void*>(remote_addr), size};
    return process_vm_writev(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

template<typename T>
T readVal (pid_t pid, uintptr_t addr) {
    T val{};
    readMem(pid, addr, &val, sizeof(T));
    return val;
}

template<typename T>
void writeVal (pid_t pid, uintptr_t addr, T val) {
    writeMem(pid, addr, &val, sizeof(T));
}

// ── Roblox helpers ───────────────────────────────────────────────────────────

std::string readName(pid_t pid, uintptr_t addr, size_t max_len = 256) {
    uint8_t fmt = 0;
    if (!readMem(pid, addr, &fmt, 1) || !(fmt >> 1)) return "";
    size_t len = std::min(static_cast<size_t>(fmt >> 1), max_len);
    std::string buf(len, '\0');
    uintptr_t src = (fmt & 1) ? (readVal<uint64_t>(pid, addr) & ~0xFFULL) : (addr + 1);
    if ((fmt & 1) && src < 0x10000) return "";
    return readMem(pid, src, buf.data(), len) ? buf : "";
}

std::string getClassByInstance (pid_t pid, uintptr_t inst) {
    uint64_t cd       = readVal<uint64_t>(pid, inst + 0x18); if (!cd)       return "";
    uint64_t name_ptr = readVal<uint64_t>(pid, cd   + 0x8);  if (!name_ptr) return "";
    return readName(pid, name_ptr);
}

std::string getNameByInstance (pid_t pid, uintptr_t inst) {
    uint64_t name = readVal<uint64_t>(pid, inst + 0xB0); if (!name) return "";
    return readName(pid, name);
}

std::vector<uint64_t> getChildrenByInstance (pid_t pid, uintptr_t inst) {
    std::vector<uint64_t> children;
    uint64_t ctl   = readVal<uint64_t>(pid, inst + 0x78); if (!ctl) return children;
    uint64_t first = readVal<uint64_t>(pid, ctl);
    uint64_t end   = readVal<uint64_t>(pid, ctl + 0x8);
    if (!first || !end) return children;
    for (uint64_t cur = first; cur != end; cur += 0x10) {
        if (uint64_t child = readVal<uint64_t>(pid, cur))
            children.push_back(child);
    }
    return children;
}

uint64_t findChildByClass (pid_t pid, uintptr_t parent, const std::string& name) {
    for (uint64_t c : getChildrenByInstance(pid, parent)) if (getClassByInstance(pid, c) == name) return c;
    return 0;
}

uint64_t findHumanoidByPlayer (pid_t pid, uint64_t player) {
    uint64_t character = Check(readVal<uint64_t>(pid, player + 0x380), "Character is null (not spawned)");
    uint64_t humanoid = Check(findChildByClass(pid, character, "Humanoid"), "Humanoid not found");
    return humanoid;
}

struct CFrameMatrix {
    float r11, r12, r13; // Rotation Row 1
    float r21, r22, r23; // Rotation Row 2
    float r31, r32, r33; // Rotation Row 3
    float x, y, z;       // Position Translation Vector
};

uint64_t getCFrameByPlayer (pid_t pid, uint64_t player) {
    uint64_t Character   = Check(readVal<uint64_t>(pid, player + 0x380), "Character is null");
    uint64_t PrimaryPart = Check(readVal<uint64_t>(pid, Character + 0x238), "PrimaryPart is null");
    uint64_t Primitive   = Check(readVal<uint64_t>(pid, PrimaryPart + 0x138), "Primitive is null");
    uint64_t CFrame      = Check(Primitive + 0xC8, "CFrame pointer is invalid");
    return CFrame;
}

void teleportToEnemy(pid_t pid, uintptr_t players, uint64_t localPlayer, const std::string& target) {
    uint64_t localCFrame = getCFrameByPlayer(pid, localPlayer);
    CFrameMatrix localMatrix = readVal<CFrameMatrix>(pid, localCFrame);
    CFrameMatrix enemyMatrix;
    bool found = false;
    for (uint64_t enemy : getChildrenByInstance(pid, players)) {
        if (getNameByInstance(pid, enemy) == target) {
            std::printf("[+] %s found\n", target.c_str());
            enemyMatrix = readVal<CFrameMatrix>(pid, getCFrameByPlayer(pid, enemy));
            found = true;
            break;
        }
    }
    if (!found) { std::printf("[-] Target not found.\n"); return; }
    std::printf("Teleporting... Before: X: %f Y: %f Z: %f\n", localMatrix.x, localMatrix.y, localMatrix.z);
    enemyMatrix.y += 5.0f;
    localMatrix = enemyMatrix; 
    for (int i = 0; i < 100000; i++) { writeVal<CFrameMatrix>(pid, localCFrame, localMatrix); }
    localMatrix = readVal<CFrameMatrix>(pid, localCFrame);
    std::printf("After: X: %f Y: %f Z: %f\n", localMatrix.x, localMatrix.y, localMatrix.z);
}


void editWalkSpeed (pid_t pid, uint64_t humanoid, float newWalkSpeed) {
    uint64_t walkSpeed = humanoid + 0x1CC;
    uint64_t walkSpeedCheck = Check(humanoid + 0x3B4, "WalkSpeedCheck not found");
    // std::printf("[+] WalkSpeed (before): %f\n", Check(readVal<float>(pid, walkSpeed), "WalkSpeed not found"));
    writeVal<float>(pid, humanoid + 0x1CC, newWalkSpeed);
    writeVal<float>(pid, walkSpeedCheck, newWalkSpeed);
    // std::printf("[+] WalkSpeed (after):  %f\n", Check(readVal<float>(pid, walkSpeedCheck), "WalkSpeedCheck not found"));
}

void editJumpHeight (pid_t pid, uint64_t humanoid, float newJumpHeight) {
    uint64_t jumpHeight = humanoid + 0x19C;
    // std::printf("[+] JumpHeight (before): %f\n", Check(readVal<float>(pid, jumpHeight), "JumpHeight not found")); 
    writeVal<float>(pid, humanoid + 0x19C, newJumpHeight); 
    // std::printf("[+] JumpHeight (after):  %f\n", readVal<float>(pid, jumpHeight));
}

// ── Process utilities ─────────────────────────────────────────────────────────

intptr_t findBase(pid_t pid) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    for (std::string line; std::getline(maps, line);) {
        if (line.find(" r-xp 00000000 ") != std::string::npos && line.find("/memfd:") != std::string::npos) {
            return std::stoull(line.substr(0, line.find('-')), nullptr, 16);
        }
    }
    return 0;
}

pid_t getPidByName(const std::string& name) {
    DIR* dir = opendir("/proc");
    if (!dir) return 0;
    pid_t pid = 0;
    while (struct dirent* entry = readdir(dir)) {
        if (int curr_pid = atoi(entry->d_name); curr_pid > 0) {
            std::ifstream comm("/proc/" + std::to_string(curr_pid) + "/comm");
            std::string line;
            if (std::getline(comm, line) && line == name) { pid = curr_pid; break; }
        }
    }
    closedir(dir);
    return pid;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    pid_t pid = Check(getPidByName("Main"), "Process 'Main' not found");
    uintptr_t base         = Check(findBase(pid), "Could not find module base");
    uint64_t fdm_ptr       = Check(readVal<uint64_t>(pid, base + 0x6BB3FD0), "FakeDataModel is null");
    uint64_t dm            = Check(readVal<uint64_t>(pid, fdm_ptr + 0x1D8), "DataModel is null");
    uint64_t players       = Check(findChildByClass(pid, dm, "Players"), "Players not found");
    uint64_t localPlayer   = Check(readVal<uint64_t>(pid, players + 0x128), "LocalPlayer is null (not in game)");
    uint64_t localHumanoid = Check(findHumanoidByPlayer(pid, localPlayer), "Humanoid not found");

    // teleportToEnemy(pid, players, localPlayer, "doyqa");
    // while (true) {
    editWalkSpeed(pid, localHumanoid, 100);
    editJumpHeight(pid, localHumanoid, 30);
    // }
    return 0;
}
