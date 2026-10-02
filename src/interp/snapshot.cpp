#include "interp/snapshot.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace rr::interp {
namespace {

// ------------------------------------------------------------------ a minimal JSON reader
//
// cpu.json is produced by our own tools\scout\savestate.py, so a small strict reader is enough;
// anything unexpected is an error rather than a default.

struct JsonValue;
using JsonPtr = std::shared_ptr<JsonValue>;

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonPtr> array;
    std::map<std::string, JsonPtr> object;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    JsonPtr Parse(std::string& error) {
        SkipWhitespace();
        JsonPtr v = ParseValue(error);
        if (!v) return nullptr;
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void SkipWhitespace() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_;
    }

    bool Fail(std::string& error, const char* what) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "JSON parse error at offset %zu: %s", i_, what);
        error = buf;
        return false;
    }

    JsonPtr ParseValue(std::string& error) {
        SkipWhitespace();
        if (i_ >= s_.size()) { Fail(error, "unexpected end of input"); return nullptr; }
        const char c = s_[i_];
        if (c == '{') return ParseObject(error);
        if (c == '[') return ParseArray(error);
        if (c == '"') {
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::String;
            if (!ParseString(v->text, error)) return nullptr;
            return v;
        }
        if (s_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::Bool;
            v->boolean = true;
            return v;
        }
        if (s_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::Bool;
            v->boolean = false;
            return v;
        }
        if (s_.compare(i_, 4, "null") == 0) {
            i_ += 4;
            return std::make_shared<JsonValue>();
        }
        {
            const size_t start = i_;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
            while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                                      s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+')) {
                ++i_;
            }
            if (i_ == start) { Fail(error, "not a value"); return nullptr; }
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::Number;
            v->number = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
            return v;
        }
    }

    bool ParseString(std::string& out, std::string& error) {
        if (s_[i_] != '"') return Fail(error, "expected a string");
        ++i_;
        out.clear();
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\') {
                ++i_;
                if (i_ >= s_.size()) return Fail(error, "unterminated escape");
                switch (s_[i_]) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (i_ + 4 >= s_.size()) return Fail(error, "truncated \\u escape");
                    out.push_back('?');
                    i_ += 4;
                    break;
                }
                default: out.push_back(s_[i_]); break;
                }
                ++i_;
            } else {
                out.push_back(s_[i_++]);
            }
        }
        if (i_ >= s_.size()) return Fail(error, "unterminated string");
        ++i_;
        return true;
    }

    JsonPtr ParseArray(std::string& error) {
        auto v = std::make_shared<JsonValue>();
        v->type = JsonValue::Type::Array;
        ++i_;
        SkipWhitespace();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return v; }
        for (;;) {
            JsonPtr e = ParseValue(error);
            if (!e) return nullptr;
            v->array.push_back(e);
            SkipWhitespace();
            if (i_ >= s_.size()) { Fail(error, "unterminated array"); return nullptr; }
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return v; }
            Fail(error, "expected , or ] in array");
            return nullptr;
        }
    }

    JsonPtr ParseObject(std::string& error) {
        auto v = std::make_shared<JsonValue>();
        v->type = JsonValue::Type::Object;
        ++i_;
        SkipWhitespace();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return v; }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key, error)) return nullptr;
            SkipWhitespace();
            if (i_ >= s_.size() || s_[i_] != ':') { Fail(error, "expected :"); return nullptr; }
            ++i_;
            JsonPtr e = ParseValue(error);
            if (!e) return nullptr;
            v->object[key] = e;
            SkipWhitespace();
            if (i_ >= s_.size()) { Fail(error, "unterminated object"); return nullptr; }
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return v; }
            Fail(error, "expected , or } in object");
            return nullptr;
        }
    }
};

const JsonValue* Member(const JsonValue& v, const std::string& key) {
    if (v.type != JsonValue::Type::Object) return nullptr;
    auto it = v.object.find(key);
    return it == v.object.end() ? nullptr : it->second.get();
}

bool Uint32Member(const JsonValue& v, const std::string& key, uint32_t& out, std::string& error) {
    const JsonValue* m = Member(v, key);
    if (m == nullptr || m->type != JsonValue::Type::Number) {
        error = "cpu.json: missing or non-numeric field '" + key + "'";
        return false;
    }
    const double d = m->number;
    out = static_cast<uint32_t>(static_cast<int64_t>(d) & 0xFFFFFFFFll);
    return true;
}

bool Uint32Array(const JsonValue& v, const std::string& key, uint32_t* out, size_t count, std::string& error) {
    const JsonValue* m = Member(v, key);
    if (m == nullptr || m->type != JsonValue::Type::Array || m->array.size() != count) {
        error = "cpu.json: field '" + key + "' must be an array of " + std::to_string(count) + " numbers";
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (m->array[i]->type != JsonValue::Type::Number) {
            error = "cpu.json: non-numeric element in '" + key + "'";
            return false;
        }
        out[i] = static_cast<uint32_t>(static_cast<int64_t>(m->array[i]->number) & 0xFFFFFFFFll);
    }
    return true;
}

std::string Join(const std::string& dir, const char* name) {
    std::string s = dir;
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s.push_back('\\');
    s += name;
    return s;
}

} // namespace

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    if (size < 0) {
        std::fclose(f);
        error = "cannot size " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_SET);
    out.assign(static_cast<size_t>(size), 0);
    const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        error = "short read of " + path;
        return false;
    }
    return true;
}

bool PlaceImage(Memory& memory, const std::vector<uint8_t>& image, uint32_t address, std::string& error) {
    if (!Memory::IsRamAddress(address)) {
        error = "PlaceImage: destination is not in guest RAM";
        return false;
    }
    const uint32_t offset = Memory::RamOffset(address);
    if (static_cast<uint64_t>(offset) + image.size() > Memory::kRamSize) {
        error = "PlaceImage: image runs past the end of guest RAM";
        return false;
    }
    std::memcpy(memory.ram().data() + offset, image.data(), image.size());
    return true;
}

bool LoadSnapshot(const std::string& directory, Memory& memory, Cpu& cpu, SnapshotInfo& info,
                  std::string& error) {
    info = SnapshotInfo{};
    info.directory = directory;

    std::vector<uint8_t> ram;
    if (!ReadWholeFile(Join(directory, "ram.bin"), ram, error)) return false;
    if (ram.size() != Memory::kRamSize) {
        error = "ram.bin is " + std::to_string(ram.size()) + " bytes, expected 2 MiB";
        return false;
    }
    memory.ram() = std::move(ram);

    std::vector<uint8_t> scratch;
    std::string ignored;
    if (ReadWholeFile(Join(directory, "scratchpad.bin"), scratch, ignored)) {
        if (scratch.size() != Memory::kScratchpadSize) {
            error = "scratchpad.bin is " + std::to_string(scratch.size()) + " bytes, expected 1 KiB";
            return false;
        }
        memory.scratchpad() = std::move(scratch);
        info.hasScratchpad = true;
    }

    std::vector<uint8_t> bios;
    if (ReadWholeFile(Join(directory, "bios.bin"), bios, ignored)) {
        if (bios.size() != Memory::kBiosSize) {
            error = "bios.bin is " + std::to_string(bios.size()) + " bytes, expected 512 KiB";
            return false;
        }
        memory.bios() = std::move(bios);
        info.hasBios = true;
    }

    std::vector<uint8_t> spuctl;
    if (ReadWholeFile(Join(directory, "spuctl.bin"), spuctl, ignored)) {
        if (spuctl.size() != sizeof(info.spuControl)) {
            error = "spuctl.bin is " + std::to_string(spuctl.size()) + " bytes, expected 32";
            return false;
        }
        for (size_t i = 0; i < 16; ++i)
            info.spuControl[i] = static_cast<uint16_t>(spuctl[2 * i] | (spuctl[2 * i + 1] << 8));
        info.hasSpuControl = true;
    }

    std::vector<uint8_t> cpuJsonBytes;
    if (!ReadWholeFile(Join(directory, "cpu.json"), cpuJsonBytes, error)) return false;
    const std::string cpuJson(reinterpret_cast<const char*>(cpuJsonBytes.data()), cpuJsonBytes.size());

    JsonParser parser(cpuJson);
    JsonPtr root = parser.Parse(error);
    if (!root || root->type != JsonValue::Type::Object) {
        if (error.empty()) error = "cpu.json is not a JSON object";
        return false;
    }

    uint32_t gpr[32];
    if (!Uint32Array(*root, "gpr_raw", gpr, 32, error)) return false;
    for (int i = 0; i < 32; ++i) cpu.regs[i] = gpr[i];
    cpu.regs[0] = 0;

    if (!Uint32Member(*root, "hi", cpu.hi, error)) return false;
    if (!Uint32Member(*root, "lo", cpu.lo, error)) return false;
    if (!Uint32Member(*root, "pc", cpu.pc, error)) return false;
    if (!Uint32Member(*root, "npc", cpu.npc, error)) return false;
    // pc == npc in every captured state, which is the
    // "resume here and fetch fresh" convention. Honour whatever the file says rather than assuming.
    info.pc = cpu.pc;
    info.npc = cpu.npc;

    const JsonValue* cop0 = Member(*root, "cop0");
    if (cop0 == nullptr) {
        error = "cpu.json: missing cop0 block";
        return false;
    }
    static const struct { const char* name; int index; } kCop0[] = {
        {"BPC", 3}, {"BDA", 5}, {"TAR", 6}, {"DCIC", 7}, {"BadVaddr", 8}, {"BDAM", 9},
        {"BPCM", 11}, {"SR", 12}, {"CAUSE", 13}, {"EPC", 14}, {"PRID", 15},
    };
    for (const auto& entry : kCop0) {
        uint32_t v = 0;
        if (!Uint32Member(*cop0, entry.name, v, error)) return false;
        cpu.cop0[entry.index] = v;
    }

    uint32_t dr[32], cr[32];
    if (!Uint32Array(*root, "gte_dr32", dr, 32, error)) return false;
    if (!Uint32Array(*root, "gte_cr32", cr, 32, error)) return false;
    for (int i = 0; i < 32; ++i) {
        cpu.gte().dr[i] = dr[i];
        cpu.gte().cr[i] = cr[i];
    }

    uint32_t loadReg = 36, loadValue = 0;
    if (!Uint32Member(*root, "load_delay_reg", loadReg, error)) return false;
    if (!Uint32Member(*root, "load_delay_value", loadValue, error)) return false;
    // The emulator that wrote the state uses 36 (= Reg::count) for "no pending load"; anything >= 32
    // means the same thing here.
    cpu.loadDelayReg = (loadReg < 32) ? static_cast<uint8_t>(loadReg) : Cpu::kNoLoadDelay;
    cpu.loadDelayValue = loadValue;

    const JsonValue* src = Member(*root, "_source");
    if (src != nullptr) {
        uint32_t frame = 0;
        std::string discard;
        if (Uint32Member(*src, "frame_number", frame, discard)) info.frameNumber = frame;
    }

    return true;
}

} // namespace rr::interp
