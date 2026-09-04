// Auto Belt Refill - D2RLoader plugin (v0.35.7)
//
// Full version history: docs/CHANGELOG.md (moved out of this source file in
// v0.35.4 to keep the source small; the file ships with the source tree).
//
// Goal: keep the consumable belt filled by pulling matching consumables out
// of the inventory. Manual mode (the fill key) and auto-refill mode (watch
// the belt, refill when a bottle is consumed) are both in.
//
// How the belt works (learned live from D2R 3.2.0 build 92777):
//   - Belt items report y == 0 and x == slot index; slot = row * 4 + column,
//     row 0 is the hotkey row (the bottom, always-visible row).
//   - The belt is organised per COLUMN and a column only accepts the consumable
//     type already sitting in it. "Is there room?" therefore cannot be answered
//     from the empty-slot count - every candidate must be offered to the game's
//     own GetFreeBeltSlot routine.
//   - BeltTransfer(0x15F660) performs the same move a player gets from
//     shift-clicking an inventory potion. It is ASYNCHRONOUS: returning true
//     only means the local game server accepted the request; the item leaves
//     the inventory page (ItemData+0x55 != 0) a few frames later. Waiting on
//     the game thread never sees the commit (the mirror updates in the game
//     thread's own pump), so the refill re-dispatches itself onto later frames
//     and only issues the next request once the previous bottle has landed.
//
// Column memory v0.33 (design doc: 方案 v3, "bottom-row anchor"):
//   - Each column holds an ORDERED list of item codes = its refill priority
//     ("hp5 hp4 hp3 hp2 hp1" = super healing first, then lesser, and so on).
//   - The list comes from exactly three sources:
//       1. the config file (consumables = [...] in auto-belt-refill.toml);
//          locked = true additionally freezes the column against every
//          in-game update;
//       2. LEARNED in-game: a placement that lands in the FIRST belt row
//          (the hotkey row - the row Blizzard's own pickup assignment keys
//          on), the snapshot key, and the login/first-fill baseline. A
//          learned list is the placed item's family, largest first when
//          prefer_large = true (hp5 before hp4, rvl before rvs);
//       3. nothing yet: the column is unassigned and never refills.
//   - Removals NEVER change the memory: drinking, shift-clicking a potion
//     back, dropping and selling are all ignored. The only in-game way to
//     change a column is to place a different consumable into its first row
//     or press the snapshot key. Memory is per-session: a new game resets the
//     learned columns (configured ones reload from the file).
//
// Console commands:
//   belt-fill     refill every empty belt slot per the column memories (key too).
//   belt-place    diagnostics: move one inventory item into the belt via the
//                 game's own slot search (see the section comment).
//   belt-policy   read-only: per-column memory + current belt contents.
//   belt-config   read-only: the parsed config file.
//   belt-snapshot re-record every column from its first row (snapshot key too).
//   belt-reset    forget the learned memories; the next fill re-records them.
//   belt-scan     read-only: dump belt + inventory layout.
//   belt-watch    read-only: hook statistics since the last invocation.
//   belt-verify   read-only: check that the hooked RVAs match this build.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <D2RLPlugin/api.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// ------------------------------------------------------------------ metadata

constexpr D2RL::PluginInfo AutoBeltRefillInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.apiVersion  = D2RL_PLUGIN_API_VERSION,
	.id          = "auto-belt-refill",
	.name        = "Auto Belt Refill",
	.version     = "0.35.7",
	.author      = "AutoBeltRefill",
	.description = "Refills empty belt slots from the inventory, per-column.",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

// ------------------------------------------------------------------- services

const D2RL::InventoryServiceV1* g_inventory = nullptr;
const D2RL::ThreadServiceV1*    g_threads   = nullptr;
const D2RL::InputServiceV1*     g_input     = nullptr;

// v0.35.7: the LIVE action bindings as the game's control settings hold them.
// The toml key is only the factory default handed to registerAction; once the
// player binds the action in the game UI, the game stores that binding in its
// own settings (it survives restarts and overrides the toml). Read back at
// load so the banner shows the truth instead of a stale toml value.
D2RL::Input::Key g_liveFillKey     = D2RL::Input::Key::None;
D2RL::Input::Key g_liveSnapshotKey = D2RL::Input::Key::None;
bool             g_liveFillValid     = false;
bool             g_liveSnapshotValid = false;

// -------------------------------------------------------------------- limits

constexpr uint32_t BeltColumns       = 4;
constexpr uint32_t MaxRowsPerColumn  = 4;
constexpr uint32_t MaxBeltSlots      = BeltColumns * MaxRowsPerColumn;
constexpr uint32_t MaxCarried        = 160;
constexpr uint32_t MaxWorn           = 24;
constexpr uint32_t PolicyMaxCodes    = 8;  // entries per column priority list
constexpr int32_t  BeltBodyLocation  = 8;

constexpr auto BeltColumnOf(uint32_t slot) noexcept -> uint32_t { return slot % BeltColumns; }
constexpr auto BeltRowOf(uint32_t slot) noexcept -> uint32_t { return slot / BeltColumns; }
constexpr auto BeltSlotOf(uint32_t row, uint32_t column) noexcept -> uint32_t { return row * BeltColumns + column; }

// --------------------------------------------------------------------- types

struct Cell {
	D2RL::ItemHandle handle = D2RL::InvalidItemHandle;
	uint32_t         code   = 0;
	uint32_t         level  = 0;
	uint32_t         seed   = 0;
	uint32_t         genSeed = 0;
	int32_t          x      = 0;
	int32_t          y      = 0;
	int32_t          page   = 0;
	int32_t          body   = 0;
	int32_t          qty    = 0;
};

struct Snapshot {
	D2RL::Inventory::Result status         = D2RL::Inventory::Result::Success;
	D2RL::PlayerHandle      player         = D2RL::InvalidPlayerHandle;
	uint32_t                beltCount      = 0;
	uint32_t                inventoryCount = 0;
	uint32_t                equipmentCount = 0;
	uint32_t                dropped        = 0;
	uint32_t                beltRows       = MaxRowsPerColumn; // rows per column; derived from equipped belt

	std::array<Cell, MaxBeltSlots> belt {};
	std::array<Cell, MaxCarried>   inventory {};
	std::array<Cell, MaxWorn>      equipment {};
};

// -------------------------------------------------------------------- helpers

// Defined with the diagnostics state below; LogDebug gates on it.
extern bool g_debugLogs;

void Log(const D2RL::PluginContext* context, const char* format, ...) noexcept {
	if (context == nullptr) {
		return;
	}

	char     buffer[512] {};
	va_list  args {};
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	context->LogInfo(buffer);
}

// v0.35.0: diagnostics that only make sense while troubleshooting (per-slot
// dumps, policy bookkeeping, hook observations). Controlled by the toml
// `debug_logs` switch so a normal play session writes only the headline
// lines (version, fill done, refill triggers, refusals, chain health).
void LogDebug(const D2RL::PluginContext* context, const char* format, ...) noexcept {
	if (context == nullptr || !g_debugLogs) {
		return;
	}

	char     buffer[512] {};
	va_list  args {};
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	context->LogInfo(buffer);
}

// A D2 item code is up to four ASCII bytes padded with spaces.
void CodeText(uint32_t code, char (&out)[5]) noexcept {
	for (uint32_t index = 0; index < 4; ++index) {
		const auto raw = static_cast<char>((code >> (index * 8U)) & 0xFFU);
		out[index] = (raw >= 0x20 && raw <= 0x7E) ? raw : '.';
	}
	out[4] = '\0';

	for (uint32_t index = 3; index > 0; --index) {
		if (out[index] == ' ') {
			out[index] = '\0';
		} else {
			break;
		}
	}
}

const char* ResultName(D2RL::Inventory::Result result) noexcept {
	switch (result) {
		case D2RL::Inventory::Result::Success:         return "Success";
		case D2RL::Inventory::Result::InvalidArgument: return "InvalidArgument";
		case D2RL::Inventory::Result::Unsupported:     return "Unsupported";
		case D2RL::Inventory::Result::Unavailable:     return "Unavailable";
		case D2RL::Inventory::Result::Conflict:        return "Conflict";
		case D2RL::Inventory::Result::NotFound:        return "NotFound";
		case D2RL::Inventory::Result::Busy:           return "Busy";
		case D2RL::Inventory::Result::OwnerInactive:   return "OwnerInactive";
		case D2RL::Inventory::Result::OwnerMismatch:   return "OwnerMismatch";
		case D2RL::Inventory::Result::StaleHandle:     return "StaleHandle";
		case D2RL::Inventory::Result::CallbackFault:   return "CallbackFault";
		case D2RL::Inventory::Result::PolicyRejected:   return "PolicyRejected";
		default:                                       return "Unknown";
	}
}

const char* ResultName(D2RL::Threads::Result result) noexcept {
	switch (result) {
		case D2RL::Threads::Result::Success:          return "Success";
		case D2RL::Threads::Result::InvalidArgument:  return "InvalidArgument";
		case D2RL::Threads::Result::Unavailable:      return "Unavailable";
		case D2RL::Threads::Result::Busy:             return "Busy";
		case D2RL::Threads::Result::OwnerInactive:    return "OwnerInactive";
		case D2RL::Threads::Result::CallbackFault:    return "CallbackFault";
		default:                                     return "Unknown";
	}
}

// Map belt item code to rows-per-column for D2R 3.2 base items.
// Observed: hbl (Plated Belt) gives 16 slots => 4 rows per column.
constexpr auto RowsForBeltCode(uint32_t code) noexcept -> uint32_t {
	switch (code) {
		// Sash / Light Belt = 8 slots => 2 rows per column
		case D2RL::Items::MakeItemCode("lbl"):
		case D2RL::Items::MakeItemCode("vbl"):
			return 2;
		// Belt / Heavy Belt = 12 slots => 3 rows per column
		case D2RL::Items::MakeItemCode("mbl"):
		case D2RL::Items::MakeItemCode("tbl"):
			return 3;
		// Plated Belt / Girdle / Uber Belt = 16 slots => 4 rows per column
		case D2RL::Items::MakeItemCode("hbl"):
		case D2RL::Items::MakeItemCode("gbl"):
		case D2RL::Items::MakeItemCode("ubl"):
			return 4;
		default:
			return 4;
	}
}

// Reverse lookup for the banner/config logs; "(custom)" for exotic keys.
// Writes into a caller buffer: two KeyName() calls must coexist in one
// snprintf, so no shared static storage.
void KeyName(D2RL::Input::Key key, char (&out)[16]) noexcept {
	const uint32_t value = static_cast<uint32_t>(key);
	if (value == 0) { // v0.35.2: unbound (fill_key = "" in the toml)
		std::snprintf(out, 16, "unbound");
		return;
	}
	if (value >= 0x70 && value <= 0x87) { // F1..F24
		std::snprintf(out, 16, "F%u", value - 0x6F);
		return;
	}
	if ((value >= 0x30 && value <= 0x39) || (value >= 0x41 && value <= 0x5A)) {
		out[0] = static_cast<char>(value);
		out[1] = '\0';
		return;
	}
	if (value == 0x20) {
		std::snprintf(out, 16, "Space");
		return;
	}
	std::snprintf(out, 16, "(custom)");
}

// -------------------------------------------------------------------- config
//
// The plugin reads config/auto-belt-refill.toml. The SDK's EnsureConfig
// creates the file from the template below on first launch; ReadConfig hands
// back its text and a hand-rolled TOML subset parser turns it into g_config.
// The subset is enough for this file: [section] headers, `key = value`
// lines, booleans, quoted strings and string arrays.

struct ColumnConfig {
	uint32_t codes[PolicyMaxCodes] {};
	uint32_t count = 0;
	bool     locked = false;
};

struct Config {
	bool             enabled     = true;
	D2RL::Input::Key fillKey     = D2RL::Input::Key::None; // v0.35.3: NO default hotkey - absent in toml = unbound
	D2RL::Input::Key snapshotKey = D2RL::Input::Key::None;
	bool             autoRefill  = false; // v0.34: watch the belt and refill on consumption
	uint32_t         autoPollFrames = 15;  // v0.34.1: frames between auto-refill polls (clamped 2..360)
	bool             debugLogs   = false; // v0.35.0: master diagnostics switch (hook probes, per-slot dumps, policy bookkeeping)
	bool             preferLarge = true;
	ColumnConfig     column[BeltColumns] {};
};

Config g_config;

const char* const DefaultConfigToml = R"toml(# Auto-Belt-Refill
# 腰带自动补货插件。修改后需重启游戏生效。
# 药水代码：hp5：超级治疗药水；hp4：高级治疗药水；hp3：治疗药水；hp2：次级治疗药水；hp1：初级治疗药水 | mp5~mp1 法力药水由高到低 | rvl大紫 rvs小紫
# 其他基础消耗品代码：vps：耐力药剂；yps：解毒药水；wms：解冻药水；tsc：城镇传送卷轴；isc：鉴定卷轴
# mod 自定义消耗品：直接填物品代码即可，无需其他设置
[general]
# 总开关。false = 插件完全停用
enabled = true
# 手动补满快捷键：按一下，按各列记忆把腰带补满。不写此行（或留 ""）= 无快捷键，
# 没有任何内置默认键；不用快捷键也能用控制台命令 belt-fill 补满
# fill_key = "F9"
# 快照快捷键：把当前腰带第一行各列的消耗品类型记为补给记忆。不写此行（或留 ""）
# = 无快捷键。第一行为空的列保持原记忆不动
# snapshot_key = "F8"
# 自动补货：开启后插件监视腰带，检测到消耗（喝掉/移走）就自动按各列记忆
# 发起一次补给（与按 F9 同一路径）。默认关闭
auto_refill = false
# 自动补货的轮询间隔，单位=帧（60fps 下 15 帧 ≈ 0.25 秒）。每次轮询只读一次
# 腰带占用并做对比，负载可忽略；数值越小反应越快。有效范围 2~360，
# 超出范围按边界取值。改完重启游戏生效
auto_poll_frames = 15
# 诊断日志总开关（默认关闭，平时不要开）：控制所有排查用日志——游戏槽位
# 查询观测、逐格腰带快照、记忆变化、每次补货请求明细。关闭时正常游玩只写
# 少量关键行（版本、补货触发与结果、拒绝原因、监控链状态），日志文件增长
# 很慢。只在补货异常需要排查时打开
debug_logs = false
# 同类优先补大号：血药 hp5 优先于 hp4，大紫 rvl 优先于小紫 rvs。
# 自动记忆的列按此规则从大到小找货；各列 consumables 列表写什么顺序就按什么顺序补
prefer_large = true
# ---------------- 每列设置 ----------------
# 不写的列 = 未分配，由游戏内行为自动记忆
# （往第一行放药 / 按 F8 快照 / 第一次按 F9 时自动记录）
# 设置第1列腰带消耗品栏位属性：
[column1]
# 给第1列补给的消耗品类型和补给优先级，从前往后，优先级依次降低
# 如在下面"[]"中填入"hp5", "hp4", "hp3", "hp2", "hp1"，则代表补给全部5种HP药剂，优先补给hp5超级治疗药水
consumables = ["hp5", "hp4"]
# 锁定本列：
#   true  = 只按上面的consumables列表补，无视游戏内一切放置、拾取、快照键
#          （ps：这一列永远是consumables内的消耗品，永不改变，玩家也无法在游戏中临时调整该列补给类型了。）
#          注意：游戏本身按每列第一行（最下面一格）的物品认定整列类型。若第一行被换成
#          别的类型消耗品，游戏会拒绝本列表药水进入该列任何空位（此为游戏基础设定，
#          并非插件限制）；往第一行放回任意一瓶列表内的药水即可恢复补给。
#          （再注意：locked=true 的列若因上述原因被游戏拒绝，插件不会越过游戏强行
#          补货——这正是"锁"的含义：只补列表内的、且游戏认可落入本列的。想让列记忆
#          自动跟随游戏路由请用 false）
#   false = 允许游戏内"往第一行放消耗品"或"按 snapshot_key快捷键"更新本列补给记忆
#          （ps：玩家在游戏中想自己改变某一列补给类型，只需要将这类消耗品放在该列最下面一格即可，或者按下snapshot_key更新当前补给类型记忆）
locked = false
# 设置第2列腰带消耗品栏位属性：
[column2]
consumables = ["mp5", "mp4"]
locked = false
# 设置第3列腰带消耗品栏位属性：
[column3]
# 只写 rvl = 这列只补大紫，小紫不进
consumables = ["rvl"]
locked = false
# 设置第4列腰带消耗品栏位属性：
[column4]
# consumables不写 = 不指定，使用游戏内自动记忆（留空 同样视为不指定）
consumables = []
locked = false
)toml";

// ------------------------------------------------- tiny text parsing helpers

constexpr auto IsBlankChar(char c) noexcept -> bool {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Case-insensitive compare of a length-delimited token against a lowercase
// literal; the literal must be all-lowercase ASCII.
auto TextEquals(const char* text, uint32_t length, const char* lower) noexcept -> bool {
	for (uint32_t index = 0; index < length; ++index) {
		char c = text[index];
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
		if (lower[index] == '\0' || c != lower[index]) {
			return false;
		}
	}
	return lower[length] == '\0';
}

auto ParseBoolValue(const char* text, uint32_t length, bool& out) noexcept -> bool {
	if (TextEquals(text, length, "true")) {
		out = true;
		return true;
	}
	if (TextEquals(text, length, "false")) {
		out = false;
		return true;
	}
	return false;
}

// A plain unsigned decimal integer ("15"). No sign, no quotes, no suffix.
auto ParseUIntValue(const char* text, uint32_t length, uint32_t& out) noexcept -> bool {
	if (length == 0 || length > 9) {
		return false;
	}
	uint32_t value = 0;
	for (uint32_t index = 0; index < length; ++index) {
		if (text[index] < '0' || text[index] > '9') {
			return false;
		}
		value = value * 10 + static_cast<uint32_t>(text[index] - '0');
	}
	out = value;
	return true;
}

// A quoted value: "F9". The input is already left-trimmed and line-trimmed.
	auto ParseQuotedValue(const char* value, uint32_t length, char* out, uint32_t outSize, uint32_t& outLen) noexcept -> bool {
	if (length < 2 || value[0] != '"') {
		return false;
	}
	uint32_t index = 1;
	for (; index < length; ++index) {
		if (value[index] == '"') {
			break;
		}
	}
	if (index == length) {
		return false; // no closing quote
	}
	const uint32_t len = index - 1;
	// v0.35.2: the empty string "" is VALID - it means "no key bound" for
	// fill_key / snapshot_key (players whose F9 is already taken).
	if (len >= outSize) {
		return false;
	}
	for (uint32_t i = 0; i < len; ++i) {
		out[i] = value[1 + i];
	}
	out[len] = '\0';
	outLen   = len;
	return true;
}

// An item-code token ("hp5") in the game's encoding: four bytes,
// little-endian, padded with ASCII spaces - exactly MakeItemCode.
auto ParseItemCode(const char* text, uint32_t length) noexcept -> uint32_t {
	if (length == 0 || length > 4) {
		return 0;
	}
	uint32_t code = 0;
	for (uint32_t index = 0; index < length; ++index) {
		const auto c = static_cast<unsigned char>(text[index]);
		if (c < 0x21 || c > 0x7E) {
			return 0;
		}
		code |= static_cast<uint32_t>(c) << (index * 8U);
	}
	for (uint32_t index = length; index < 4; ++index) {
		code |= 0x20U << (index * 8U);
	}
	return code;
}

// Key names accepted in the config: "F1".."F24", "0".."9", "a".."z",
// "space", "insert", "delete", "home", "end", "pageup", "pagedown".
auto ParseKeyName(const char* text, uint32_t length) noexcept -> D2RL::Input::Key {
	if (length == 1) {
		const char c = text[0];
		if (c >= '0' && c <= '9') {
			return static_cast<D2RL::Input::Key>(0x30 + (c - '0'));
		}
		if (c >= 'a' && c <= 'z') {
			return static_cast<D2RL::Input::Key>(0x41 + (c - 'a'));
		}
		if (c >= 'A' && c <= 'Z') {
			return static_cast<D2RL::Input::Key>(0x41 + (c - 'A'));
		}
		return D2RL::Input::Key::None;
	}
	if (length == 2 && (text[0] == 'f' || text[0] == 'F') && text[1] >= '1' && text[1] <= '9') {
		return static_cast<D2RL::Input::Key>(0x70 + (text[1] - '1'));
	}
	if (length == 3 && (text[0] == 'f' || text[0] == 'F') && text[1] >= '1' && text[1] <= '2' && text[2] >= '0' && text[2] <= '4') {
		return static_cast<D2RL::Input::Key>(0x70 + 9 + (text[1] - '1') * 10 + (text[2] - '0'));
	}
	if (TextEquals(text, length, "space"))    return D2RL::Input::Key::Space;
	if (TextEquals(text, length, "insert"))    return D2RL::Input::Key::Insert;
	if (TextEquals(text, length, "delete"))    return D2RL::Input::Key::Delete;
	if (TextEquals(text, length, "home"))      return D2RL::Input::Key::Home;
	if (TextEquals(text, length, "end"))       return D2RL::Input::Key::End;
	if (TextEquals(text, length, "pageup"))    return D2RL::Input::Key::PageUp;
	if (TextEquals(text, length, "pagedown"))  return D2RL::Input::Key::PageDown;
	return D2RL::Input::Key::None;
}

// A consumables array: ["hp5", "hp4"] or []. Both quoted and bare tokens are
// accepted so hand-edited files without quotes keep working.
auto ParseCodeArray(const char* value, uint32_t length, ColumnConfig& column) noexcept -> bool {
	if (length < 2 || value[0] != '[') {
		return false;
	}
	uint32_t index = 1;
	while (index < length && value[index] != ']') {
		while (index < length && (IsBlankChar(value[index]) || value[index] == ',')) {
			++index;
		}
		if (index >= length || value[index] == ']') {
			break;
		}

		const char* tokenStart = value + index;
		uint32_t    tokenLen   = 0;
		if (value[index] == '"') {
			++index;
			tokenStart = value + index;
			while (index < length && value[index] != '"' && value[index] != ']') {
				++index;
			}
			tokenLen = static_cast<uint32_t>(value + index - tokenStart);
			if (index < length && value[index] == '"') {
				++index;
			}
		} else {
			while (index < length && value[index] != ',' && value[index] != ']' && !IsBlankChar(value[index])) {
				++index;
			}
			tokenLen = static_cast<uint32_t>(value + index - tokenStart);
		}

		if (tokenLen > 0 && column.count < PolicyMaxCodes) {
			const uint32_t code = ParseItemCode(tokenStart, tokenLen);
			if (code != 0) {
				column.codes[column.count++] = code;
			}
		}
	}
	return true;
}

// Line-based parser for the subset this plugin's file uses. Unknown keys and
// malformed lines are counted (and logged by the caller) but never abort the
// parse - a partially hand-edited file still yields its valid values.
auto ParseConfig(const char* text) noexcept -> uint32_t {
	uint32_t issues  = 0;
	int32_t  section = -1; // -1 none, 0 general, 1..4 column
	uint32_t line    = 0;
	(void)line;

	const char* cursor = text;
	while (*cursor != '\0') {
		const char* start = cursor;
		while (*cursor != '\0' && *cursor != '\n') {
			++cursor;
		}
		const char* end = cursor;
		if (*cursor == '\n') {
			++cursor;
		}
		++line;

		// Strip comments that live outside quoted strings.
		bool inQuote = false;
		for (const char* p = start; p < end; ++p) {
			if (*p == '"') {
				inQuote = !inQuote;
			} else if (*p == '#' && !inQuote) {
				end = p;
				break;
			}
		}

		while (start < end && IsBlankChar(*start)) {
			++start;
		}
		while (end > start && IsBlankChar(*(end - 1))) {
			--end;
		}
		if (start == end) {
			continue;
		}

		// [section]
		if (*start == '[') {
			const char* close = start + 1;
			while (close < end && *close != ']') {
				++close;
			}
			if (close == end) {
				++issues;
				continue;
			}
			const char* name     = start + 1;
			const uint32_t nameLen = static_cast<uint32_t>(close - name);
			if (TextEquals(name, nameLen, "general")) {
				section = 0;
			} else if (nameLen == 7 && TextEquals(name, 6, "column")
			           && name[6] >= '1' && name[6] <= '4') {
				section = name[6] - '0';
			} else {
				section = -1;
				++issues;
			}
			continue;
		}

		// key = value
		const char* eq = start;
		while (eq < end && *eq != '=') {
			++eq;
		}
		if (eq == end) {
			++issues;
			continue;
		}
		const char* keyStart = start;
		const char* keyEnd   = eq;
		while (keyEnd > keyStart && IsBlankChar(*(keyEnd - 1))) {
			--keyEnd;
		}
		const char* valueStart = eq + 1;
		while (valueStart < end && IsBlankChar(*valueStart)) {
			++valueStart;
		}
		const uint32_t keyLen   = static_cast<uint32_t>(keyEnd - keyStart);
		const uint32_t valueLen = static_cast<uint32_t>(end - valueStart);
		if (keyLen == 0 || valueLen == 0) {
			++issues;
			continue;
		}

		if (section == 0) {
			if (TextEquals(keyStart, keyLen, "enabled")) {
				if (!ParseBoolValue(valueStart, valueLen, g_config.enabled)) {
					++issues;
				}
			} else if (TextEquals(keyStart, keyLen, "auto_refill")) {
				if (!ParseBoolValue(valueStart, valueLen, g_config.autoRefill)) {
					++issues;
				}
			} else if (TextEquals(keyStart, keyLen, "auto_poll_frames")) {
				if (ParseUIntValue(valueStart, valueLen, g_config.autoPollFrames)) {
					// Clamp: below 2 the dispatch chain hogs the frame
					// budget for no gain; above 360 (6 s) the watcher is
					// too slow to matter.
					if (g_config.autoPollFrames < 2) {
						g_config.autoPollFrames = 2;
					} else if (g_config.autoPollFrames > 360) {
						g_config.autoPollFrames = 360;
					}
				} else {
					++issues;
				}
			} else if (TextEquals(keyStart, keyLen, "debug_logs") || TextEquals(keyStart, keyLen, "debug_hooks")) {
				if (!ParseBoolValue(valueStart, valueLen, g_config.debugLogs)) {
					++issues;
				}
			} else if (TextEquals(keyStart, keyLen, "prefer_large")) {
				if (!ParseBoolValue(valueStart, valueLen, g_config.preferLarge)) {
					++issues;
				}
			} else if (TextEquals(keyStart, keyLen, "fill_key") || TextEquals(keyStart, keyLen, "snapshot_key")) {
				char     token[16] {};
				uint32_t tokenLen = 0;
				if (!ParseQuotedValue(valueStart, valueLen, token, sizeof(token), tokenLen)) {
					++issues;
					continue;
				}
				const D2RL::Input::Key key = ParseKeyName(token, tokenLen);
				// v0.35.2: an empty string ("") unbinds the key; a NON-empty
				// name that does not parse is still an error and leaves the
				// key unbound (v0.35.3: there is no built-in default).
				if (key == D2RL::Input::Key::None && tokenLen != 0) {
					++issues;
					continue;
				}
				if (TextEquals(keyStart, keyLen, "fill_key")) {
					g_config.fillKey = key;
				} else {
					g_config.snapshotKey = key;
				}
			} else {
				++issues;
			}
		} else if (section >= 1 && section <= static_cast<int32_t>(BeltColumns)) {
			ColumnConfig& column = g_config.column[section - 1];
			if (TextEquals(keyStart, keyLen, "consumables")) {
				column.count = 0;
				if (!ParseCodeArray(valueStart, valueLen, column)) {
					++issues;
				}
			} else if (TextEquals(keyStart, keyLen, "locked")) {
				if (!ParseBoolValue(valueStart, valueLen, column.locked)) {
					++issues;
				}
			} else {
				++issues;
			}
		}
	}
	return issues;
}

// Creates the file on first launch, reads it back and parses it. Malformed
// lines keep the built-in defaults; the issue count is logged once.
void LoadConfig(const D2RL::PluginContext* context) noexcept {
	if (!context->EnsureConfig(DefaultConfigToml)) {
		Log(context, "AutoBeltRefill: config: EnsureConfig failed - using built-in defaults.");
		return;
	}

	char     buffer[16384] {};
	uint32_t required = 0;
	if (!context->ReadConfig(buffer, sizeof(buffer) - 1, &required)) {
		Log(context, "AutoBeltRefill: config: ReadConfig failed - using built-in defaults.");
		return;
	}
	buffer[sizeof(buffer) - 1] = '\0';

	const uint32_t issues = ParseConfig(buffer);
	if (issues != 0) {
		Log(context, "AutoBeltRefill: config: %u unparsable line(s) skipped.", issues);
	}
}

// -------------------------------------------------------------------- scanning

auto __cdecl CollectCell(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* userData) noexcept -> D2RL::Inventory::IterationAction {
	auto* snapshot = static_cast<Snapshot*>(userData);
	if (snapshot == nullptr || item == nullptr || item->structSize < D2RL::Items::ItemInfoRequiredSize) {
		return D2RL::Inventory::IterationAction::Stop;
	}

	const Cell cell {
		.handle  = item->handle,
		.code    = item->code,
		.level   = item->itemLevel,
		.seed    = item->itemSeed,
		.genSeed = item->generationSeed,
		.x       = item->x,
		.y       = item->y,
		.page    = item->inventoryPage,
		.body    = item->bodyLocation,
		.qty     = item->quantity,
	};

	switch (item->container) {
		case D2RL::Items::ItemContainer::Belt:
			if (snapshot->beltCount < snapshot->belt.size()) {
				snapshot->belt[snapshot->beltCount++] = cell;
			} else {
				++snapshot->dropped;
			}
			break;
		case D2RL::Items::ItemContainer::Inventory:
			if (snapshot->inventoryCount < snapshot->inventory.size()) {
				snapshot->inventory[snapshot->inventoryCount++] = cell;
			} else {
				++snapshot->dropped;
			}
			break;
		case D2RL::Items::ItemContainer::Equipment:
			if (snapshot->equipmentCount < snapshot->equipment.size()) {
				snapshot->equipment[snapshot->equipmentCount++] = cell;
			} else {
				++snapshot->dropped;
			}
			break;
		default:
			break;
	}

	return D2RL::Inventory::IterationAction::Continue;
}

auto DetermineBeltRows(const Snapshot& snapshot) noexcept -> uint32_t {
	// Prefer the equipped belt code.
	for (uint32_t index = 0; index < snapshot.equipmentCount; ++index) {
		const Cell& cell = snapshot.equipment[index];
		if (cell.body == BeltBodyLocation && cell.code != 0) {
			return RowsForBeltCode(cell.code);
		}
	}
	// Fallback: if the belt is full or nearly full, max slot index + 1 gives capacity.
	uint32_t maxSlot = 0;
	for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
		if (snapshot.belt[index].x > 0 && static_cast<uint32_t>(snapshot.belt[index].x) > maxSlot) {
			maxSlot = static_cast<uint32_t>(snapshot.belt[index].x);
		}
	}
	const uint32_t fallbackRows = (maxSlot + BeltColumns) / BeltColumns; // round up to whole columns
	if (fallbackRows >= 1 && fallbackRows <= MaxRowsPerColumn) {
		return fallbackRows;
	}
	return MaxRowsPerColumn;
}

auto Gather(const D2RL::PluginContext* context, Snapshot& snapshot) noexcept -> bool {
	if (g_inventory->getLocalPlayer(context, &snapshot.player) != D2RL::Inventory::Result::Success) {
		Log(context, "AutoBeltRefill: no local player. Join a game first.");
		return false;
	}

	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Belt)
		               | D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory)
		               | D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Equipment),
	};

	snapshot.status = g_inventory->forEachInventoryItem(context, snapshot.player, &filter, CollectCell, &snapshot);
	if (snapshot.status != D2RL::Inventory::Result::Success) {
		Log(context, "AutoBeltRefill: enumeration failed (%s).", ResultName(snapshot.status));
		return false;
	}

	snapshot.beltRows = DetermineBeltRows(snapshot);
	return true;
}

void DumpBelt(const D2RL::PluginContext* context, const Snapshot& snapshot) noexcept {
	const uint32_t rows = snapshot.beltRows;
	Log(context, "AutoBeltRefill: belt holds %u item(s), rows per column = %u, total slots = %u.", snapshot.beltCount, rows, BeltColumns * rows);

	// Occupancy map indexed by slot.
	bool taken[MaxBeltSlots] {};
	for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
		const Cell& cell = snapshot.belt[index];
		if (cell.x >= 0 && static_cast<uint32_t>(cell.x) < MaxBeltSlots) {
			taken[cell.x] = true;
		}
	}

	for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
		const Cell& cell = snapshot.belt[index];
		const auto slot = static_cast<uint32_t>(cell.x);
		const uint32_t column = BeltColumnOf(slot);
		const uint32_t row    = BeltRowOf(slot);
		char           text[5] {};
		CodeText(cell.code, text);
		Log(context, "AutoBeltRefill:   [belt slot %2u] col=%u row=%u code=%-4s (%08X) qty=%d ilvl=%u", slot, column, row, text, cell.code, cell.qty, cell.level);
	}

	Log(context, "AutoBeltRefill: belt grid (bottom row is the hotkey row, '.' is empty):");
	for (uint32_t row = 0; row < rows; ++row) {
		char line[256] {};
		int  written = std::snprintf(line, sizeof(line), "AutoBeltRefill:   row %u |", row);
		bool rowUsed = false;
		for (uint32_t column = 0; column < BeltColumns; ++column) {
			const uint32_t slot = BeltSlotOf(row, column);
			char           text[5] {};
			if (taken[slot]) {
				for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
					if (snapshot.belt[index].x == static_cast<int32_t>(slot)) {
						CodeText(snapshot.belt[index].code, text);
						rowUsed = true;
						break;
					}
				}
			} else {
				std::snprintf(text, sizeof(text), ".");
			}
			written += std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written), " %-4s", text);
		}
		if (rowUsed) {
			context->LogInfo(line);
		}
	}
}

void DumpInventory(const D2RL::PluginContext* context, const Snapshot& snapshot) noexcept {
	Log(context, "AutoBeltRefill: inventory holds %u item(s).", snapshot.inventoryCount);
	for (uint32_t index = 0; index < snapshot.inventoryCount; ++index) {
		const Cell& cell = snapshot.inventory[index];
		char        text[5] {};
		CodeText(cell.code, text);
		Log(context, "AutoBeltRefill:   [inv %2u] code=%-4s (%08X) page=%d x=%d y=%d qty=%d", index, text, cell.code, cell.page, cell.x, cell.y, cell.qty);
	}
}

void DumpEquipment(const D2RL::PluginContext* context, const Snapshot& snapshot) noexcept {
	Log(context, "AutoBeltRefill: equipment holds %u item(s).", snapshot.equipmentCount);
	for (uint32_t index = 0; index < snapshot.equipmentCount; ++index) {
		const Cell& cell = snapshot.equipment[index];
		char        text[5] {};
		CodeText(cell.code, text);
		Log(context, "AutoBeltRefill:   [worn] code=%-4s (%08X) bodyLocation=%d", text, cell.code, cell.body);
	}
}

// ----------------------------------------------------------------------- fill

auto FindSlot(const Snapshot& snapshot, uint32_t slot) noexcept -> const Cell* {
	for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
		if (snapshot.belt[index].x == static_cast<int32_t>(slot)) {
			return &snapshot.belt[index];
		}
	}
	return nullptr;
}

// Returns the dominant non-zero code in the given column, or 0 if the column is empty / mixed.
auto ColumnTargetCode(const Snapshot& snapshot, uint32_t column, uint32_t rows) noexcept -> uint32_t {
	uint32_t codeCounts[MaxBeltSlots] {};
	uint32_t codeSlots[MaxBeltSlots] {};
	uint32_t distinctCount = 0;

	for (uint32_t row = 0; row < rows; ++row) {
		const uint32_t slot = BeltSlotOf(row, column);
		const Cell*    cell = FindSlot(snapshot, slot);
		if (cell == nullptr || cell->code == 0) {
			continue;
		}

		uint32_t existing = 0;
		for (uint32_t index = 0; index < distinctCount; ++index) {
			if (codeCounts[index] == cell->code) {
				++codeSlots[index];
				existing = codeSlots[index];
				break;
			}
		}
		if (existing == 0 && distinctCount < MaxBeltSlots) {
			codeCounts[distinctCount] = cell->code;
			codeSlots[distinctCount]  = 1;
			++distinctCount;
		}
	}

	uint32_t bestCode  = 0;
	uint32_t bestCount = 0;
	for (uint32_t index = 0; index < distinctCount; ++index) {
		if (codeSlots[index] > bestCount) {
			bestCount = codeSlots[index];
			bestCode  = codeCounts[index];
		}
	}
	return bestCode;
}

// v0.33: there is no hardcoded consumable whitelist any more. A code is a
// refill candidate exactly when some column's memory list contains it -
// which is also how mod consumables get in: put one in a first-row slot (or
// list its code in the config) and the column wants it.

// ------------------------------------------------------------ native routing
//
// The SDK cannot write to the belt (ItemContainer::Belt is rejected as a
// create/transaction destination), so the refill drives the game's own
// routines directly. All RVAs are for D2R 3.2.0 build 92777 and are guarded
// by byte-pattern checks before each hook is installed.

constexpr uint64_t GetLocalDataContextRva = 0x08B2D0;
constexpr uint64_t GetLocalPlayerRva      = 0x09A480;
constexpr uint64_t GetUnitInventoryRva     = 0x34A360;
constexpr uint64_t GetItemDataRva          = 0x34A500;
constexpr uint64_t GetUnitTypeRva          = 0x34B9D0;
constexpr uint64_t GetFirstItemRva         = 0x388C10;
constexpr uint64_t GetNextItemRva          = 0x38ABA0;
constexpr uint64_t GetItemCodeRva          = 0x36EF50;
constexpr uint64_t GetFreeBeltSlotRva      = 0x3862D0;
constexpr uint64_t CanPutInBeltRva         = 0x15A110;
constexpr uint64_t BeltTransferRva         = 0x15F660;
constexpr uint64_t PickupRva                = 0x471950;

constexpr uint64_t ItemDataPageOffset = 0x55; // byte: inventory page id; 0 = main inventory, 0xFF = belt/equipment
constexpr uint32_t ItemUnitType       = 4;

// The game asks this routine which belt slot a potion should use. We hook it
// both to observe the real placement paths (for the upcoming automatic mode)
// and to call the original ourselves when probing whether a candidate fits.
constexpr uint8_t GetFreeBeltSlotBytes[32] = {
	0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
	0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x70, 0x01,
	0x00, 0x00, 0x48, 0x8B, 0x05, 0xDF, 0x4F, 0x64,
	0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x84, 0x24
};

constexpr uint8_t CanPutInBeltBytes[32] = {
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
	0xD9, 0xE8, 0x22, 0x9D, 0x21, 0x00, 0x85, 0xC0,
	0x74, 0x40, 0xE8, 0xA9, 0x11, 0xF3, 0xFF, 0x8B,
	0xC8, 0xE8, 0x52, 0x03, 0xF4, 0xFF, 0x48, 0x8B
};

constexpr uint8_t BeltTransferBytes[32] = {
	0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x57, 0x48,
	0x83, 0xEC, 0x60, 0x48, 0x8B, 0x05, 0x56, 0xBC,
	0x86, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x44,
	0x24, 0x40, 0x48, 0x8B, 0xB4, 0x24, 0xB0, 0x00
};

constexpr uint8_t PickupBytes[32] = {
	0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
	0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
	0x48, 0x8D, 0x6C, 0x24, 0xE9, 0x48, 0x81, 0xEC,
	0xA0, 0x00, 0x00, 0x00, 0x45, 0x33, 0xE4, 0x44
};

using GetLocalDataContextFn = int32_t(__fastcall*)() noexcept;
using GetLocalPlayerFn      = void*(__fastcall*)(int32_t) noexcept;
using GetUnitInventoryFn    = void*(__fastcall*)(void*) noexcept;
using GetItemDataFn         = void*(__fastcall*)(void*) noexcept;
using GetUnitTypeFn         = int32_t(__fastcall*)(void*) noexcept;
using GetFirstItemFn        = void*(__fastcall*)(void*) noexcept;
using GetNextItemFn         = void*(__fastcall*)(void*) noexcept;
using GetItemCodeFn         = uint32_t(__fastcall*)(void*) noexcept;
using GetFreeBeltSlotFn     = int32_t(__fastcall*)(void* inventory, void* item, int32_t* freeSlot, bool allowAnyBeltable) noexcept;
using CanPutInBeltFn        = bool(__fastcall*)(void* item) noexcept;
using BeltTransferFn        = bool(__fastcall*)(void* item, void* player, uint8_t a3, uint8_t a4, void* out) noexcept;
using PickupFn              = bool(__fastcall*)(void* player, uint32_t guid, bool arg3, uint32_t arg4, bool arg5, bool arg6) noexcept;

GetLocalDataContextFn GetLocalDataContext = nullptr;
GetLocalPlayerFn      GetLocalPlayer      = nullptr;
GetUnitInventoryFn    GetUnitInventory    = nullptr;
GetItemDataFn         GetItemData         = nullptr;
GetUnitTypeFn          GetUnitType         = nullptr;
GetFirstItemFn        GetFirstItem         = nullptr;
GetNextItemFn         GetNextItem          = nullptr;
GetItemCodeFn         GetItemCode          = nullptr;
GetFreeBeltSlotFn     OriginalGetFreeBeltSlot = nullptr;
CanPutInBeltFn        OriginalCanPutInBelt    = nullptr;
BeltTransferFn        OriginalBeltTransfer    = nullptr;
PickupFn              OriginalPickup          = nullptr;

// Set at load so the hooks can log raw RVAs.
uintptr_t                  g_moduleBase     = 0;
const D2RL::PluginContext* g_hookLogContext = nullptr;
uint32_t                   g_hookHits       = 0;
bool                       g_debugLogs      = false; // v0.35.0: master diagnostics switch, synced from g_config.debugLogs after load

auto ResolveNatives(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || g_moduleBase == 0) {
		return false;
	}

	GetLocalDataContext = reinterpret_cast<GetLocalDataContextFn>(g_moduleBase + GetLocalDataContextRva);
	GetLocalPlayer     = reinterpret_cast<GetLocalPlayerFn>(g_moduleBase + GetLocalPlayerRva);
	GetUnitInventory   = reinterpret_cast<GetUnitInventoryFn>(g_moduleBase + GetUnitInventoryRva);
	GetItemData        = reinterpret_cast<GetItemDataFn>(g_moduleBase + GetItemDataRva);
	GetUnitType        = reinterpret_cast<GetUnitTypeFn>(g_moduleBase + GetUnitTypeRva);
	GetFirstItem       = reinterpret_cast<GetFirstItemFn>(g_moduleBase + GetFirstItemRva);
	GetNextItem        = reinterpret_cast<GetNextItemFn>(g_moduleBase + GetNextItemRva);
	return GetUnitInventory != nullptr && GetFirstItem != nullptr && GetItemData != nullptr;
}

// ------------------------------------------------------------------ policy
//
// Each belt column remembers an ORDERED list of item codes - its refill
// priority. Rank 0 is tried first; when that code runs dry the refill
// falls through to rank 1, and so on. A learned list is the placed item's
// family, largest first when prefer_large = true (hp5 before hp4, rvl
// before rvs); a configured list is exactly what the file says.
//
// Writers (design doc 方案 v3, "bottom-row anchor"):
//   a. the config file (ApplyConfiguredPolicies) - the only writer that
//      may set locked, which freezes the column against every in-game
//      update;
//   b. LEARNED in-game (SetLearnedPolicy): a placement that lands in the
//      FIRST belt row - the hotkey row, the row Blizzard's own pickup
//      assignment keys on - re-records the column. That covers manual
//      drag-and-drop, shift-click, shop bulk-buy and pickup auto-assign
//      alike, plus the snapshot key and the login baseline, which read
//      the first row directly;
//   c. nothing yet: the column is unassigned and never refills.
//
// Removals NEVER change a memory: drinking, taking a potion back out,
// dropping and selling are all ignored. The only in-game way to change a
// column is to place a different consumable into its first row or press
// the snapshot key. Memory is per-session: a new game clears the learned
// columns; configured ones reload from the file.

enum class Family : uint8_t {
	None,          // not a consumable / never refilled
	Blood,         // hp1..hp5
	Mana,          // mp1..mp5
	Rejuvenation,  // rvs, rvl
	Thawing,       // wms
	Antidote,      // yps
	Stamina,       // vps
	TownPortal,    // tsc
	Identify,      // isc
	Specific,      // anything else (mod consumables): exact-code matching only
};

constexpr auto FamilyOf(uint32_t code) noexcept -> Family {
	if (code == 0) {
		return Family::None;
	}
	const auto first  = static_cast<char>((code >> 0U)  & 0xFFU);
	const auto second = static_cast<char>((code >> 8U) & 0xFFU);
	if (first == 'h' && second == 'p') {
		return Family::Blood;
	}
	if (first == 'm' && second == 'p') {
		return Family::Mana;
	}
	if (first == 'r' && second == 'v') {
		return Family::Rejuvenation;
	}
	switch (code) {
		case D2RL::Items::MakeItemCode("wms"): return Family::Thawing;
		case D2RL::Items::MakeItemCode("yps"): return Family::Antidote;
		case D2RL::Items::MakeItemCode("vps"): return Family::Stamina;
		case D2RL::Items::MakeItemCode("tsc"): return Family::TownPortal;
		case D2RL::Items::MakeItemCode("isc"): return Family::Identify;
		default:                               return Family::Specific;
	}
}

struct ColumnPolicy {
	bool     initialized = false; // a refill list is active
	bool     locked      = false; // config lock: in-game updates ignored
	uint32_t codes[PolicyMaxCodes] {};
	uint32_t count       = 0;
};

ColumnPolicy g_policy[BeltColumns] {};

D2RL::PlayerHandle g_policyPlayer = D2RL::InvalidPlayerHandle; // game-change detection

// "hp5 > hp4 > hp3" for the logs ("unassigned" when empty).
void DescribeCodeList(const ColumnPolicy& policy, char (&out)[96]) noexcept {
	if (!policy.initialized || policy.count == 0) {
		std::snprintf(out, 96, "unassigned");
		return;
	}
	uint32_t used = 0;
	out[0] = '\0';
	for (uint32_t index = 0; index < policy.count && used + 8 < 96; ++index) {
		char text[5] {};
		CodeText(policy.codes[index], text);
		const int written = std::snprintf(out + used, 96 - used, index == 0 ? "%s" : " > %s", text);
		if (written <= 0 || used + static_cast<uint32_t>(written) >= 96) {
			break;
		}
		used += static_cast<uint32_t>(written);
	}
}

// Four ASCII bytes, little-endian, padded with spaces - the game's item-code
// encoding (same as MakeItemCode, but usable with runtime-built characters).
constexpr auto MakeCode(char a, char b, char c) noexcept -> uint32_t {
	return static_cast<uint32_t>(static_cast<unsigned char>(a))
	     | (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8U)
	     | (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16U)
	     | (0x20U << 24U);
}

// Expands a family into its ordered code list. preferLarge = true puts the
// biggest member first (hp5 before hp4, rvl before rvs); false reverses it.
// Fixed families hold a single code; Specific is expanded by the caller.
auto FamilyList(Family family, bool preferLarge, uint32_t (&codes)[PolicyMaxCodes]) noexcept -> uint32_t {
	uint32_t count = 0;
	switch (family) {
		case Family::Blood:
		case Family::Mana: {
			const char first = (family == Family::Blood) ? 'h' : 'm';
			for (uint32_t step = 0; step < 5 && count < PolicyMaxCodes; ++step) {
				const char digit = preferLarge ? static_cast<char>('5' - step) : static_cast<char>('1' + step);
				codes[count++] = MakeCode(first, 'p', digit);
			}
			return count;
		}
		case Family::Rejuvenation:
			codes[count++] = preferLarge ? MakeCode('r', 'v', 'l') : MakeCode('r', 'v', 's');
			codes[count++] = preferLarge ? MakeCode('r', 'v', 's') : MakeCode('r', 'v', 'l');
			return count;
		case Family::Thawing:    codes[count++] = MakeCode('w', 'm', 's'); return count;
		case Family::Antidote:   codes[count++] = MakeCode('y', 'p', 's'); return count;
		case Family::Stamina:    codes[count++] = MakeCode('v', 'p', 's'); return count;
		case Family::TownPortal: codes[count++] = MakeCode('t', 's', 'c'); return count;
		case Family::Identify:   codes[count++] = MakeCode('i', 's', 'c'); return count;
		default:
			return 0;
	}
}

// Records a column memory from a single placed code. Locked columns return
// early (the config owns them); a rewrite that lands on the same list is a
// no-op. Logs actual transitions only, to keep the log clean.
void SetLearnedPolicy(uint32_t column, uint32_t code, const char* source) noexcept {
	if (column >= BeltColumns) {
		return;
	}
	auto& policy = g_policy[column];
	if (policy.locked) {
		return;
	}
	const Family family = FamilyOf(code);
	if (family == Family::None) {
		return;
	}

	uint32_t codes[PolicyMaxCodes] {};
	uint32_t count = (family == Family::Specific) ? 0 : FamilyList(family, g_config.preferLarge, codes);
	if (family == Family::Specific) {
		codes[0] = code;
		count    = 1;
	}

	bool changed = !policy.initialized || policy.count != count;
	if (!changed) {
		for (uint32_t index = 0; index < count; ++index) {
			if (policy.codes[index] != codes[index]) {
				changed = true;
				break;
			}
		}
	}
	if (!changed) {
		return;
	}

	policy.initialized = true;
	policy.count       = count;
	for (uint32_t index = 0; index < count; ++index) {
		policy.codes[index] = codes[index];
	}

	if (g_hookLogContext != nullptr) {
		char description[96] {};
		DescribeCodeList(policy, description);
		LogDebug(g_hookLogContext,
			"AutoBeltRefill: policy: column %u <= %s (via %s).",
			column,
			description,
			source);
	}
}

// Copies the configured columns into the live policies. count == 0 means
// "not specified": the column stays whatever it is (usually unassigned,
// ready to learn in-game). Called on load and after every player change.
void ApplyConfiguredPolicies() noexcept {
	for (uint32_t column = 0; column < BeltColumns; ++column) {
		const ColumnConfig& config = g_config.column[column];
		if (config.count == 0) {
			continue;
		}
		ColumnPolicy& policy = g_policy[column];
		policy.initialized   = true;
		policy.locked        = config.locked;
		policy.count         = config.count;
		for (uint32_t index = 0; index < config.count; ++index) {
			policy.codes[index] = config.codes[index];
		}
	}
}

// Belt identity for the diff layer: code + seed + generation seed stay stable
// while the item moves between the belt and the inventory.
struct BeltRecord {
	uint32_t code    = 0;
	uint32_t seed    = 0;
	uint32_t genSeed = 0;
	int32_t  slot    = 0;
};

constexpr auto SameIdentity(const Cell& cell, uint32_t code, uint32_t seed, uint32_t genSeed) noexcept -> bool {
	return cell.code == code && cell.seed == seed && cell.genSeed == genSeed;
}

BeltRecord g_lastBelt[MaxBeltSlots] {};
uint32_t   g_lastBeltCount = 0;
bool       g_lastBeltValid = false;

// Login baseline + diff maintenance. Runs at the start of every fill session
// and on demand (belt-policy). The FIRST belt row is the anchor: only it
// teaches, and the baseline reads it directly.
void UpdatePoliciesFromSnapshot(const D2RL::PluginContext* context, const Snapshot& snapshot) noexcept {
	// A different player means a new game: learned columns are forgotten,
	// configured columns come back from the file. The very FIRST sight of a
	// player is not a change, though (v0.33.1): g_policyPlayer starts as
	// InvalidPlayerHandle, so treating every first observation as a change
	// wiped everything recorded before the first fill of a process - the
	// snapshot key, row-0 placements, a belt-policy read. Only a genuine
	// in-process player switch resets; the first observation just tracks.
	if (snapshot.player != g_policyPlayer) {
		if (g_policyPlayer != D2RL::InvalidPlayerHandle) {
			for (uint32_t column = 0; column < BeltColumns; ++column) {
				g_policy[column] = ColumnPolicy {};
			}
			ApplyConfiguredPolicies();
			LogDebug(context, "AutoBeltRefill: policy: new player - learned columns reset (configured columns restored).");
		} else {
			LogDebug(context, "AutoBeltRefill: policy: player tracked - memories recorded earlier are kept.");
		}
		g_lastBeltCount = 0;
		g_lastBeltValid = false;
		g_policyPlayer  = snapshot.player;
	}

	// (a) baseline: every column that has never been recorded reads its code
	// from the first row (slot == column). An empty first row leaves the
	// column unassigned - it never refills until something lands in row 0.
	for (uint32_t column = 0; column < BeltColumns; ++column) {
		if (g_policy[column].initialized) {
			continue;
		}
		const Cell* cell = FindSlot(snapshot, column);
		if (cell != nullptr && cell->code != 0 && FamilyOf(cell->code) != Family::None) {
			SetLearnedPolicy(column, cell->code, "login baseline");
		}
	}

	if (!g_lastBeltValid) {
		// First snapshot of this game: remember it, diff starts next time.
		g_lastBeltCount = snapshot.beltCount;
		if (g_lastBeltCount > MaxBeltSlots) {
			g_lastBeltCount = MaxBeltSlots;
		}
		for (uint32_t index = 0; index < g_lastBeltCount; ++index) {
			g_lastBelt[index].code    = snapshot.belt[index].code;
			g_lastBelt[index].seed    = snapshot.belt[index].seed;
			g_lastBelt[index].genSeed = snapshot.belt[index].genSeed;
			g_lastBelt[index].slot    = snapshot.belt[index].x;
		}
		g_lastBeltValid = true;
		return;
	}

	// (c) diff against the previous snapshot. Removals NEVER change a
	// memory (v0.33) - they are logged and ignored. Whether the bottle was
	// drunk, taken back to the inventory, sold or dropped makes no
	// difference: the column keeps its list.
	for (uint32_t index = 0; index < g_lastBeltCount; ++index) {
		const BeltRecord& record = g_lastBelt[index];

		bool stillInBelt = false;
		for (uint32_t scan = 0; scan < snapshot.beltCount; ++scan) {
			if (SameIdentity(snapshot.belt[scan], record.code, record.seed, record.genSeed)) {
				stillInBelt = true;
				break;
			}
		}
		if (stillInBelt) {
			continue;
		}

		char text[5] {};
		CodeText(record.code, text);
		LogDebug(context,
			"AutoBeltRefill: policy: column %u keeps its memory (%s left the belt - removals never change it).",
			BeltColumnOf(static_cast<uint32_t>(record.slot)),
			text);
	}

	// New identities teach only when they landed in the FIRST row. A code
	// the column already lists teaches nothing - and that is also exactly
	// the signature of our own refill landing in row 0, which must never
	// rewrite a configured priority list.
	for (uint32_t scan = 0; scan < snapshot.beltCount; ++scan) {
		const Cell& cell = snapshot.belt[scan];
		if (cell.x < 0 || cell.x >= static_cast<int32_t>(MaxBeltSlots)) {
			continue;
		}
		bool known = false;
		for (uint32_t index = 0; index < g_lastBeltCount; ++index) {
			if (SameIdentity(cell, g_lastBelt[index].code, g_lastBelt[index].seed, g_lastBelt[index].genSeed)) {
				known = true;
				break;
			}
		}
		if (known) {
			continue;
		}
		const uint32_t slot = static_cast<uint32_t>(cell.x);
		if (slot >= BeltColumns) {
			continue; // upper rows never teach (bottom-row anchor rule)
		}
		const ColumnPolicy& policy = g_policy[slot];
		bool alreadyListed = false;
		for (uint32_t index = 0; index < policy.count; ++index) {
			if (policy.codes[index] == cell.code) {
				alreadyListed = true;
				break;
			}
		}
		if (!alreadyListed) {
			SetLearnedPolicy(slot, cell.code, "first-row placement");
		}
	}

	g_lastBeltCount = snapshot.beltCount;
	if (g_lastBeltCount > MaxBeltSlots) {
		g_lastBeltCount = MaxBeltSlots;
	}
	for (uint32_t index = 0; index < g_lastBeltCount; ++index) {
		g_lastBelt[index].code    = snapshot.belt[index].code;
		g_lastBelt[index].seed    = snapshot.belt[index].seed;
		g_lastBelt[index].genSeed = snapshot.belt[index].genSeed;
		g_lastBelt[index].slot    = snapshot.belt[index].x;
	}
}

// The bottle our own refill has just requested. The BeltTransfer hook
// compares against it to tell our transfers from player actions; FillStep
// sets it BEFORE calling the original routine.
void* g_fillPending = nullptr;

// ---------------------------------------------------------------------- hooks

// Every distinct return address that reaches GetFreeBeltSlot, with a hit
// count. The per-frame UI query (0x15A156) fires hundreds of times a second,
// so dedupe by caller instead of logging raw calls. Whatever else shows up is
// a real placement path - the signal the automatic mode will be built on.
constexpr uint32_t MaxBeltCallers = 64;
uint64_t g_beltCallers[MaxBeltCallers] {};
uint32_t g_beltCallerHits[MaxBeltCallers] {};
uint32_t g_beltCallerCount = 0;
uint32_t g_beltCallsSeen   = 0;

auto __fastcall HookGetFreeBeltSlot(
	void*    inventory,
	void*    item,
	int32_t* freeSlot,
	bool     allowAnyBeltable) noexcept -> int32_t {
	const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_moduleBase;
	++g_beltCallsSeen;
	if (g_beltCallerCount > 0 || g_moduleBase != 0) {
		uint32_t slot = MaxBeltCallers;
		for (uint32_t index = 0; index < g_beltCallerCount; ++index) {
			if (g_beltCallers[index] == caller) {
				slot = index;
				break;
			}
		}
		if (slot == MaxBeltCallers && g_beltCallerCount < MaxBeltCallers) {
			slot = g_beltCallerCount;
			g_beltCallers[slot] = caller;
			++g_beltCallerCount;
		}
		if (slot < MaxBeltCallers) {
			++g_beltCallerHits[slot];
		}
	}

	if (g_debugLogs && g_hookLogContext != nullptr && g_hookHits < 12) {
		++g_hookHits;

		uint32_t itemCode = 0;
		if (item != nullptr && GetItemCode != nullptr) {
			itemCode = GetItemCode(item);
		}
		char codeText[5] {};
		CodeText(itemCode, codeText);

		Log(g_hookLogContext,
			"AutoBeltRefill: hook: call #%u caller=0x%llX inv=%p item=%p(%s) out=%p allow=%d.",
			g_hookHits,
			static_cast<unsigned long long>(caller),
			inventory,
			item,
			codeText,
			static_cast<void*>(freeSlot),
			allowAnyBeltable ? 1 : 0);
	}

	if (OriginalGetFreeBeltSlot != nullptr) {
		return OriginalGetFreeBeltSlot(inventory, item, freeSlot, allowAnyBeltable);
	}
	return 0;
}

// Observation probe on the belt-availability test. Whoever calls it is a real
// placement path - useful context while building the automatic mode.
uint32_t g_canHits = 0;

auto __fastcall HookCanPutInBelt(void* item) noexcept -> bool {
	if (g_debugLogs && g_hookLogContext != nullptr && g_canHits < 10) {
		++g_canHits;
		const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
		Log(g_hookLogContext,
			"AutoBeltRefill: canbelt: call #%u caller=0x%llX item=%p.",
			g_canHits,
			static_cast<unsigned long long>(caller - g_moduleBase),
			item);
	}
	if (OriginalCanPutInBelt != nullptr) {
		return OriginalCanPutInBelt(item);
	}
	return false;
}

// Logging hook on the game's own Pickup routine (the auto-pickup path; the
// player's manual click-to-pickup does not go through it).
uint32_t g_pickupHits = 0;

auto __fastcall HookPickup(
	void* player, uint32_t guid, bool arg3, uint32_t arg4, bool arg5, bool arg6) noexcept -> bool {
	bool result = false;
	if (OriginalPickup != nullptr) {
		result = OriginalPickup(player, guid, arg3, arg4, arg5, arg6);
	}
	if (g_debugLogs && g_hookLogContext != nullptr && g_pickupHits < 40) {
		++g_pickupHits;
		const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_moduleBase;
		Log(g_hookLogContext,
			"AutoBeltRefill: pick: #%u caller=0x%llX player=%p guid=%u a3=%d a4=%u a5=%d a6=%d -> %d.",
			g_pickupHits,
			static_cast<unsigned long long>(caller),
			player,
			guid,
			arg3 ? 1 : 0,
			arg4,
			arg5 ? 1 : 0,
			arg6 ? 1 : 0,
			result ? 1 : 0);
	}
	return result;
}

// Logging hook on the belt-transfer routine - the exact move the refill
// drives. Seeing what the game itself passes during a real shift-click
// documents the calling convention for the automatic mode.
uint32_t g_beltTransferHits = 0;

auto __fastcall HookBeltTransfer(
	void* item, void* arg2, uint8_t arg3, uint8_t arg4, void* out) noexcept -> bool {
	uint8_t before[8] {};
	uint8_t after[8]  {};
	if (out != nullptr) {
		__try {
			std::memcpy(before, out, sizeof(before));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			// Leave the snapshot zeroed; the call is still worth logging.
		}
	}

	bool result = false;
	if (OriginalBeltTransfer != nullptr) {
		result = OriginalBeltTransfer(item, arg2, arg3, arg4, out);
	}

	if (out != nullptr) {
		__try {
			std::memcpy(after, out, sizeof(after));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			// Ignore; the before/after comparison simply stays incomplete.
		}
	}

	// (b) real-time policy update: a transfer that PUT something into a
	// FIRST-ROW slot re-records the destination column (the bottom-row
	// anchor rule - manual drag, shift-click, shop bulk-buy and pickup
	// auto-assign all land here). arg4 encodes the direction: 1 = into the
	// belt (placement), 0 = out of the belt. Out-of-belt moves are ignored
	// entirely: removals never change a memory (v0.33). Our own refills are
	// filtered out via the g_fillPending pointer FillStep sets in advance.
	if (result && out != nullptr && item != nullptr && item != g_fillPending
	    && arg4 != 0) {
		__try {
			int32_t slot = 0;
			std::memcpy(&slot, out, sizeof(slot));
			if (slot >= 0 && slot < static_cast<int32_t>(BeltColumns)) {
				uint32_t code = 0;
				if (GetItemCode != nullptr) {
					code = GetItemCode(item);
				}
				SetLearnedPolicy(BeltColumnOf(static_cast<uint32_t>(slot)), code, "belt hook");
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			// A failed policy probe must never block the real transfer.
		}
	}

	if (g_debugLogs && g_hookLogContext != nullptr && g_beltTransferHits < 40) {
		++g_beltTransferHits;
		const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_moduleBase;

		uint32_t itemCode = 0;
		if (item != nullptr && GetItemCode != nullptr) {
			__try {
				itemCode = GetItemCode(item);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				itemCode = 0;
			}
		}
		char codeText[5] {};
		CodeText(itemCode, codeText);

		Log(g_hookLogContext,
			"AutoBeltRefill: belt: #%u caller=0x%llX item=%p(%s) a2=%p a3=%u a4=%u out=%p -> %d.",
			g_beltTransferHits,
			static_cast<unsigned long long>(caller),
			item,
			codeText,
			arg2,
			static_cast<uint32_t>(arg3),
			static_cast<uint32_t>(arg4),
			out,
			result ? 1 : 0);
		Log(g_hookLogContext,
			"AutoBeltRefill: belt: out before=%02X %02X %02X %02X %02X %02X %02X %02X after=%02X %02X %02X %02X %02X %02X %02X %02X.",
			before[0], before[1], before[2], before[3], before[4], before[5], before[6], before[7],
			after[0], after[1], after[2], after[3], after[4], after[5], after[6], after[7]);
	}
	return result;
}

// ---------------------------------------------------------------------- watch

// Prints every distinct caller that reached GetFreeBeltSlot since the last
// invocation, then resets the counters.
void RunWatch(const D2RL::PluginContext* context, void*) noexcept {
	if (context == nullptr) {
		return;
	}
	Log(context,
		"AutoBeltRefill: watch: %u call(s) from %u caller(s); pickup hits=%u; belt-transfer hits=%u.",
		g_beltCallsSeen, g_beltCallerCount, g_pickupHits, g_beltTransferHits);
	for (uint32_t index = 0; index < g_beltCallerCount; ++index) {
		Log(context, "AutoBeltRefill: watch: caller 0x%llX hits=%u.",
			static_cast<unsigned long long>(g_beltCallers[index]),
			g_beltCallerHits[index]);
	}

	g_beltCallerCount  = 0;
	g_beltCallsSeen    = 0;
	g_pickupHits       = 0;
	g_beltTransferHits = 0;
	for (uint32_t index = 0; index < MaxBeltCallers; ++index) {
		g_beltCallers[index]    = 0;
		g_beltCallerHits[index] = 0;
	}
}

// ---------------------------------------------------------------------- refill

// The real refill. BeltTransfer(0x15F660) performs exactly the move the player
// gets from shift-clicking an inventory potion, and the hook captured a real
// invocation to copy:
//
//     BeltTransfer(item=hp4, player=<local player>, a3=0, a4=1, out=&slot)
//         -> true, out.slot = 12
//
// a2 is the player unit: 0x15F57C compares its argument against
// GetLocalPlayer()'s return value. The function resolves the inventory itself,
// so all we supply is the item, the player and an out struct whose byte at +4
// must start at zero (a non-zero value makes it skip the slot search).
//
// The move is asynchronous AND the commit only becomes visible once the game
// thread returns to its own pump - spinning with Sleep(1) on the game thread
// never saw a commit, while the next key press, ~1.3 s later, always found
// the bottle landed. So the refill is a frame-to-frame continuation: request
// one bottle, re-dispatch ourselves onto the game thread, and only request
// the next bottle once the previous one has really left the inventory page.

constexpr uint32_t FillMaxMoves   = MaxBeltSlots; // one belt's worth
constexpr uint32_t FillMaxRetries = 150;          // ~2.5 s at 60 fps per bottle
constexpr uint32_t FillMaxBlocked = 16;           // refused bottles per session

// g_fillPending now lives with the column-policy state above the hooks.
uint32_t g_fillMoves      = 0;       // committed moves in this session
uint32_t g_fillRetries    = 0;       // consecutive no-progress dispatches
bool     g_fillRunning    = false;   // a fill session is in progress
bool     g_fillInCallback = false;   // re-entry detector for runOnGameThread
bool     g_fillIntroDone  = false;   // belt/policy log printed once per session
void*    g_fillBlocked[FillMaxBlocked] {}; // bottles the game refused this session
uint32_t g_fillBlockedCount = 0;

// v0.34.2: auto-refill timing, shared by FillStep (re-arm on timeout) and
// the watcher. The windows are TIME-based, not poll-count based: with the
// old fixed 2-poll debounce a 5-frame poll fired ~0.17 s after the drink -
// inside the game's own column-shift processing - and the server silently
// dropped the refill request (the "requested but never landed" timeout).
constexpr uint32_t AutoPollEveryDefault = 15;
// Trailing debounce window before a confirmed consumption triggers a pass,
// derived from the poll interval so its wall-clock time stays ~0.5 s at any
// poll rate (ceil of 30 frames, minimum 2 polls).
constexpr uint32_t AutoDebouncePollsFor(uint32_t pollFrames) noexcept {
	const uint32_t frames = pollFrames == 0 ? AutoPollEveryDefault : pollFrames;
	const uint32_t polls  = (30 + frames - 1) / frames;
	return polls < 2 ? 2 : polls;
}
// Quiet time after a triggered pass (~1 s; ceil of 60 frames, minimum 2).
constexpr uint32_t AutoCooldownPollsFor(uint32_t pollFrames) noexcept {
	const uint32_t frames = pollFrames == 0 ? AutoPollEveryDefault : pollFrames;
	const uint32_t polls  = (60 + frames - 1) / frames;
	return polls < 2 ? 2 : polls;
}
// Bounded re-arms for a pass that timed out with slots still unfilled.
// Reset whenever a fresh consumption opens a window.
constexpr uint32_t AutoMaxRearms  = 3;
uint32_t           g_autoRearmCount = 0;
// Defined with the watcher state below; FillStep's timeout re-arm needs it.
extern int32_t g_autoArmLeft;

// Per-column occupancy frozen at the fill session's first frame. NO fill
// (auto or manual F9 - the rules are identical since v0.35.0) introduces a
// NEW code into a column that was COMPLETELY empty when the pass started -
// such a column has no family anchor, and whatever the game routes into it
// would silently re-learn the column (the "dropped my last healing potion,
// got mana potions" report).
bool g_fillColumnHadItems[BeltColumns] = {};

// Defined with the watcher state below; FillStep's 0-moved ending sets it.
extern bool g_autoNeedsNewEvent;

// Defined further down, next to the console-command helpers.
auto Dispatch(const D2RL::PluginContext* context, D2RL::Threads::Callback callback) noexcept -> D2RL::Threads::Result;

void RunNativeFill(const D2RL::PluginContext* context, void*) noexcept;

void EnsureAutoWatch(const D2RL::PluginContext* context) noexcept;

// One step of the refill. Runs on the game thread.
void FillStep(const D2RL::PluginContext* context) noexcept {
	if (context == nullptr || !ResolveNatives(context)) {
		Log(context, "AutoBeltRefill: fill: natives not resolved.");
		g_fillRunning = false;
		return;
	}
	if (OriginalBeltTransfer == nullptr) {
		Log(context, "AutoBeltRefill: fill: BeltTransfer hook not installed.");
		g_fillRunning = false;
		return;
	}

	void* player    = nullptr;
	void* inventory = nullptr;
	__try {
		player = GetLocalPlayer(GetLocalDataContext());
		if (player != nullptr) {
			inventory = GetUnitInventory(player);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		inventory = nullptr;
	}
	if (player == nullptr || inventory == nullptr) {
		Log(context, "AutoBeltRefill: fill: no player or inventory.");
		g_fillRunning = false;
		return;
	}

	// Fresh session: maintain the column policies first (login baseline +
	// diff against the previous snapshot), then show the belt as we see it.
	// The intro flag survives refused bottles (moves can stay 0), so the
	// belt dump prints once per session, not once per retry.
	if (!g_fillIntroDone) {
		g_fillIntroDone = true;
		Snapshot snapshot {};
		for (uint32_t column = 0; column < BeltColumns; ++column) {
			// Conservative default: treat every column as anchored until the
			// snapshot proves otherwise (a failed Gather then refuses to
			// re-anchor columns instead of hijacking them).
			g_fillColumnHadItems[column] = true;
		}
		if (Gather(context, snapshot)) {
			UpdatePoliciesFromSnapshot(context, snapshot);
			bool occupied[MaxBeltSlots] {};
			char codes[MaxBeltSlots][5] {};
			for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
				const int32_t slot = snapshot.belt[index].x;
				if (slot >= 0 && slot < static_cast<int32_t>(MaxBeltSlots)) {
					occupied[slot] = true;
					CodeText(snapshot.belt[index].code, codes[slot]);
				}
			}
			for (uint32_t column = 0; column < BeltColumns; ++column) {
				bool hadItems = false;
				for (int32_t row = 0; row < static_cast<int32_t>(MaxBeltSlots) / BeltColumns; ++row) {
					const int32_t slot = row * BeltColumns + static_cast<int32_t>(column);
					hadItems = hadItems || (slot < static_cast<int32_t>(MaxBeltSlots) && occupied[slot]);
				}
				g_fillColumnHadItems[column] = hadItems;
			}
			const int32_t capacity = static_cast<int32_t>(snapshot.beltRows) * BeltColumns;
			for (int32_t slot = 0; slot < capacity; ++slot) {
				LogDebug(context, "AutoBeltRefill: fill: belt[%d] = %s.", slot, occupied[slot] ? codes[slot] : "-");
			}
			LogDebug(context,
					"AutoBeltRefill: fill: beltCount=%u rows=%u capacity=%d.",
				snapshot.beltCount,
				snapshot.beltRows,
				capacity);
			for (uint32_t column = 0; column < BeltColumns; ++column) {
				char description[96] {};
				DescribeCodeList(g_policy[column], description);
				LogDebug(context, "AutoBeltRefill: fill: policy: column %u = %s%s.", column, description,
					g_policy[column].locked ? " (locked)" : "");
			}
		}
	}

	// Continuation step 1: has our previous request landed?
	if (g_fillPending != nullptr) {
		bool committed = false;
		__try {
			const auto* data = static_cast<const uint8_t*>(GetItemData(g_fillPending));
			committed = (data == nullptr) || (data[ItemDataPageOffset] != 0);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			committed = false;
		}
		if (!committed) {
			++g_fillRetries;
			if (g_fillRetries > FillMaxRetries) {
			Log(context,
				"AutoBeltRefill: fill: previous move still pending after %u frames - stopping.",
				FillMaxRetries);
			g_fillRunning = false;
			g_fillPending = nullptr;
			g_fillRetries = 0;
			EnsureAutoWatch(context); // v0.34.4: revive the watcher if its chain died
				// v0.34.2: the game ACCEPTED the request but the bottle
				// never landed (typically the refill racing the drink's
				// own column shift). The empty slot produces no fresh
				// occupied -> empty transition, so the watcher would never
				// re-trigger it. Re-open the debounce window for the
				// still-armed slots; bounded so a pathological game state
				// cannot loop us forever.
				if (g_autoRearmCount < AutoMaxRearms) {
					++g_autoRearmCount;
					g_autoArmLeft = static_cast<int32_t>(AutoDebouncePollsFor(g_config.autoPollFrames));
					Log(context,
						"AutoBeltRefill: auto: re-arming the unfilled slot(s) (retry %u/%u).",
						g_autoRearmCount,
						AutoMaxRearms);
				}
				return;
			}
			Dispatch(context, RunNativeFill);
			return;
		}

		++g_fillMoves;
		g_fillRetries = 0;
		g_fillPending = nullptr;
		LogDebug(context, "AutoBeltRefill: fill: move committed (%u done).", g_fillMoves);
	}

	if (g_fillMoves >= FillMaxMoves) {
		Log(context, "AutoBeltRefill: fill: done, %u potion(s) moved.", g_fillMoves);
		g_fillRunning = false;
		if (g_fillMoves == 0) {
			g_autoNeedsNewEvent = true; // v0.35.0: see the 0-moved note below
		}
		EnsureAutoWatch(context); // v0.34.4: revive the watcher if its chain died
		return;
	}

	// D2R's belt is organised per column and a column only accepts the
	// consumable type already sitting in it, so "is there room?" cannot be
	// answered from the raw empty-slot count - ask the game about each
	// candidate instead. On top of that the column memory gates the refill:
	// the offered slot's column must list the candidate's code, and the
	// candidate with the LOWEST rank wins ("same family, biggest first"
	// falls out of this naturally because learned lists are ordered
	// big-to-small and configured lists are in file order).

	void*   chosen    = nullptr;
	char    codeText[5] {};
	int32_t probeSlot = -1;
	int32_t bestRank  = static_cast<int32_t>(PolicyMaxCodes) + 1;

	// v0.33.2 (a): stop-reason diagnostics. blocked* remembers the first
	// wanted item the game routed into a column whose memory does not list
	// it (the deadlock signature); noSlotCount counts wanted items the
	// game offered no slot at all. Logging only - no behavior change.
	char     blockedCode[5] {};
	int32_t  blockedSlot   = -1;
	uint32_t blockedCount  = 0;
	uint32_t noSlotCount   = 0;
	// v0.34.4: first potion the AUTO pass refused to push into a fully
	// empty column (that would hijack the column and re-learn it).
	char     emptyRefusedCode[5] {};
	int32_t  emptyRefusedSlot = -1;
	uint32_t emptyRefusedCount = 0;

	__try {
		for (void* item = GetFirstItem(inventory); item != nullptr; item = GetNextItem(item)) {
			if (GetUnitType(item) != ItemUnitType) {
				continue;
			}
			const auto* data = static_cast<const uint8_t*>(GetItemData(item));
			if (data == nullptr || data[ItemDataPageOffset] != 0) {
				continue;
			}
			const uint32_t code = GetItemCode(item);
			if (code == 0) {
				continue;
			}

			// Refused earlier in this session: the game will only turn the
			// same bottle down again, so skip it instead of re-offering it.
			bool refused = false;
			for (uint32_t blocked = 0; blocked < g_fillBlockedCount; ++blocked) {
				if (g_fillBlocked[blocked] == item) {
					refused = true;
					break;
				}
			}
			if (refused) {
				continue;
			}

			// Quick reject before probing the game: the code must appear in
			// at least one column's list.
			bool wantedByAny = false;
			for (uint32_t column = 0; column < BeltColumns && !wantedByAny; ++column) {
				const ColumnPolicy& policy = g_policy[column];
				for (uint32_t index = 0; index < policy.count; ++index) {
					if (policy.codes[index] == code) {
						wantedByAny = true;
						break;
					}
				}
			}
			if (!wantedByAny) {
				continue;
			}

			int32_t slot = -1;
			const int32_t ok = (OriginalGetFreeBeltSlot != nullptr)
				? OriginalGetFreeBeltSlot(inventory, item, &slot, true)
				: 0;
			if (ok <= 0 || slot < 0) {
				++noSlotCount; // wanted, but the game offered no slot
				continue;
			}

			// Which column did the game offer, and at what priority rank?
			const uint32_t offeredColumn = BeltColumnOf(static_cast<uint32_t>(slot));
			ColumnPolicy&  offeredPolicy = g_policy[offeredColumn];
			int32_t rank = -1;
			for (uint32_t index = 0; index < offeredPolicy.count; ++index) {
				if (offeredPolicy.codes[index] == code) {
					rank = static_cast<int32_t>(index);
					break;
				}
			}
			if (rank < 0) {
				// v0.34.2: the game routed this code into a column whose
				// memory does not list it. The game's own routing is the
				// only authority the server honors (the E1 verdict), so an
				// unlocked column ACCEPTS the offer and extends its memory
				// with the code - the game just said this column is where
				// this potion belongs now. Listed codes keep their better
				// ranks; the learned code lands at the end. A locked (or
				// full) column keeps its exact list - that is the lock.
				if (offeredPolicy.locked || offeredPolicy.count >= PolicyMaxCodes) {
					++blockedCount;
					if (blockedSlot < 0) {
						blockedSlot = slot;
						CodeText(code, blockedCode);
					}
					continue; // the column does not take this code
				}
				// v0.35.0: this rule now applies to EVERY fill (manual F9
				// included) so manual and auto behave identically - the
				// player asked for one consistent mechanic, no escape hatch.
				if (!g_fillColumnHadItems[offeredColumn]) {
					// v0.34.4: no fill may introduce a new code into a
					// column that was COMPLETELY empty when the pass
					// started - the game would place it there (an empty
					// column accepts anything), the follow-the-game rule
					// would then re-learn the column, and the player's
					// healing column could silently become a mana column.
					// The column keeps its memory; it refills again once
					// the player anchors it (first row / snapshot key).
					++emptyRefusedCount;
					if (emptyRefusedSlot < 0) {
						emptyRefusedSlot = slot;
						CodeText(code, emptyRefusedCode);
					}
					continue;
				}
				rank = static_cast<int32_t>(offeredPolicy.count);
				offeredPolicy.codes[offeredPolicy.count++] = code;
				char learnedCode[5] {};
				CodeText(code, learnedCode);
				LogDebug(context,
					"AutoBeltRefill: fill: the game routed %s into column %u - column memory extended to follow the game.",
					learnedCode,
					offeredColumn);
			}
			if (rank < bestRank) {
				bestRank  = rank;
				chosen    = item;
				probeSlot = slot;
				CodeText(code, codeText);
				if (rank == 0) {
					break; // nothing can beat rank 0
				}
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		chosen = nullptr;
	}

	if (chosen == nullptr) {
		// v0.33.2 (a): stop-reason diagnostics. The deadlock signature is a
		// wanted item the game routed into a column whose memory does not
		// list it - name the first example and point at the two in-game
		// fixes. Pure logging: nothing about the refill itself changes.
		if (blockedCount > 0) {
			char snapName[16] {};
			KeyName(g_config.snapshotKey, snapName);
			Log(context,
				"AutoBeltRefill: fill: note: %u wanted item(s) were refused by a locked (or full) column that does not list them (first: %s -> slot %d, column %u).",
				blockedCount,
				blockedCode,
				blockedSlot,
				BeltColumnOf(static_cast<uint32_t>(blockedSlot)));
			Log(context,
				"AutoBeltRefill: fill: note: a locked column only ever takes its listed codes - check that column's locked setting and its first row, or press %s to re-snapshot.",
				snapName);
		} else if (noSlotCount > 0 && g_fillMoves == 0) {
			Log(context,
				"AutoBeltRefill: fill: note: %u wanted item(s) in the inventory but the game offered no belt slot for them.",
				noSlotCount);
		}
		if (emptyRefusedCount > 0) {
			// v0.34.4: say why an auto pass left an empty column alone.
			Log(context,
				"AutoBeltRefill: fill: note: %u potion(s) were NOT pushed into the fully empty column %u (first: %s) - the column keeps its memory; place a matching potion into its first row (or press the snapshot key) to re-anchor it.",
				emptyRefusedCount,
				BeltColumnOf(static_cast<uint32_t>(emptyRefusedSlot)),
				emptyRefusedCode);
		}
		Log(context, "AutoBeltRefill: fill: done, %u potion(s) moved.", g_fillMoves);
		g_fillRunning = false;
		if (g_fillMoves == 0) {
			// v0.35.0: a 0-moved pass leaves its trigger state unchanged
			// (the empty slot and the inventory stock are both still there).
			// Lock the state-based trigger until fresh belt activity, or it
			// would re-fire on the same state after every cooldown.
			g_autoNeedsNewEvent = true;
		}
		EnsureAutoWatch(context); // v0.34.4: revive the watcher if its chain died
		return;
	}

	// Issue ONE request, remember the bottle, and let the continuation check
	// it before anything else is touched. Never ask twice for the same bottle.
	// g_fillPending is set BEFORE the call so the BeltTransfer hook can tell
	// this request apart from a player action.
	uint8_t out[32] {};
	bool    moved = false;
	bool    threw = false;
	g_fillPending = chosen;
	__try {
		moved = OriginalBeltTransfer(chosen, player, 0, 1, out);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		moved = false;
		threw = true;
	}
	if (!moved) {
		g_fillPending = nullptr;
	}

	int32_t landed = 0;
	uint8_t flag   = 0;
	std::memcpy(&landed, out, sizeof(landed));
	std::memcpy(&flag, out + 4, sizeof(flag));

	LogDebug(context,
		"AutoBeltRefill: fill: %s -> slot %d: %s (landed %d, flag %u).",
		codeText,
		probeSlot,
		threw ? "EXCEPTION" : (moved ? "requested" : "refused"),
		landed,
		static_cast<uint32_t>(flag));

	if (!moved) {
		// The game turned THIS bottle down (a cursor drag mid-press, a
		// column-type clash, ...). That used to abort the whole fill;
		// v0.33.1 blacklists it for the rest of the session and looks for
		// the next candidate instead. Only a full blacklist (or no
		// candidate left) ends the fill with fewer moves.
		if (g_fillBlockedCount < FillMaxBlocked) {
			g_fillBlocked[g_fillBlockedCount++] = chosen;
			LogDebug(context, "AutoBeltRefill: fill: bottle blacklisted for this fill - trying the next candidate.");
			Dispatch(context, RunNativeFill);
			return;
		}
		Log(context, "AutoBeltRefill: fill: blacklist full - stopping.");
		Log(context, "AutoBeltRefill: fill: done, %u potion(s) moved.", g_fillMoves);
		g_fillRunning = false;
		if (g_fillMoves == 0) {
			g_autoNeedsNewEvent = true; // v0.35.0: see the 0-moved note above
		}
		EnsureAutoWatch(context); // v0.34.4: revive the watcher if its chain died
		return;
	}

	Dispatch(context, RunNativeFill);
}

// Entry point handed to runOnGameThread. Also detects whether the dispatch
// service queues us for a later frame (good) or runs us synchronously (then
// frame-to-frame continuation is impossible and we degrade to one bottle per
// key press).
void RunNativeFill(const D2RL::PluginContext* context, void*) noexcept {
	if (g_fillInCallback) {
		// Synchronous dispatch: we were re-entered before the outer call
		// returned, so there is no frame boundary between us and the pump.
		g_fillRunning = false;
		Log(context, "AutoBeltRefill: fill: dispatch is synchronous - one bottle per press.");
		return;
	}

	g_fillInCallback = true;
	FillStep(context);
	g_fillInCallback = false;
}

// ---------------------------------------------------------------- auto refill
//
// v0.34: the drinking watcher. One self-rescheduling game-thread task polls
// the belt occupancy (the same Gather the fill uses) every AutoPollEvery
// dispatch ticks and diffs it against the previous poll. When a slot turned
// empty - with the game's column shift a drunk bottle always shows up as the
// TOP slot of its column going empty - a short trailing debounce window
// opens; drinks that land within the window merge into ONE refill pass, and
// manual refills that land there simply cancel it. If the candidates are
// still empty when the window closes, the ordinary fill session starts, so
// auto mode rides the exact natural-routing path F9 uses.
//
// The diff cannot tell WHY a slot emptied and does not need to: any empty
// wanted slot is refill-worthy. A cooldown after every triggered pass
// prevents machine-gun re-triggers while that pass is still landing.
// All timing constants are [PLACEHOLDER] until a live session tunes them.

// v0.34.2: the poll/debounce/cooldown timing constants moved up next to
// FillStep (AutoPollEveryDefault, AutoDebouncePollsFor, AutoCooldownPollsFor)
// so the timeout re-arm there can use them. The windows are time-based now:
// ~0.5 s debounce, ~1 s cooldown, whatever the poll rate.

// The monitor chain is alive (a dispatch is outstanding). Set true only by
// EnsureAutoWatch after a successful dispatch, set false only when a
// reschedule fails (game gone) or on unload - never inside the callback, or
// an interleaved EnsureAutoWatch could double-queue the chain.
bool                    g_autoChainLive   = false;
uint32_t                g_autoTickCount   = 0;  // dispatch ticks since the last poll
bool                    g_autoPrevValid   = false;
bool                    g_autoPrev[MaxBeltSlots] = {};   // previous poll's occupancy
int32_t                 g_autoArmLeft     = 0;  // polls left in the debounce window
bool                    g_autoArmed[MaxBeltSlots] = {};  // candidate slots being debounced
uint32_t                g_autoCooldownLeft = 0; // polls left before a new trigger may fire
// v0.35.0: set when a pass ended with 0 potions moved while its trigger
// condition was still true (empty slot + matching inventory potion). The
// state trigger below would otherwise re-fire on the same unchanged state
// forever; any occupancy transition (drink, place, remove) clears it.
bool                    g_autoNeedsNewEvent = false;
// Liveness stamp: GetTickCount64() of the last RunAutoWatch tick. A dispatch
// accepted right before a game transition can be silently dropped (02:49:44),
// leaving g_autoChainLive stuck true with no actual chain - EnsureAutoWatch
// uses this stamp to detect and repair that.
uint64_t                g_autoLastTickMs  = 0;
constexpr uint64_t      AutoWatchStaleAfterMs = 3000; // no tick for 3 s = dead chain

// Starts a fill session from any trigger (key, console command, auto mode).
// The caller checks the result to report a missing game thread.
auto StartFillSession(const D2RL::PluginContext* context) noexcept -> D2RL::Threads::Result {
	g_fillPending      = nullptr;
	g_fillMoves        = 0;
	g_fillRetries      = 0;
	g_fillRunning      = true;
	g_fillIntroDone    = false;
	g_fillBlockedCount = 0;
	return Dispatch(context, RunNativeFill);
}

// One monitor tick. Runs on the game thread.
void RunAutoWatch(const D2RL::PluginContext* context, void*) noexcept {
	g_autoLastTickMs = GetTickCount64(); // liveness proof for EnsureAutoWatch
	// A tick that cannot reschedule itself ends the chain; the GameJoined
	// listener (and every manual fill) starts a new one.
	auto reschedule = [&]() noexcept {
		if (Dispatch(context, RunAutoWatch) != D2RL::Threads::Result::Success) {
			// v0.34.4: this used to die silently - the 02:10 session showed
			// a dead chain with NO log line and no way to tell.
			Log(context, "AutoBeltRefill: auto: dispatch failed - watcher chain stopped; a fill or a game join restarts it.");
			g_autoChainLive = false;
			g_autoPrevValid = false;
			g_autoArmLeft   = 0;
			g_autoTickCount = 0;
		}
	};

	if (!g_config.autoRefill) {
		// A disabled toml ends the chain here instead of idling once per
		// frame. The flag is only flipped at load time, so this runs once
		// per chain start at most.
		Log(context, "AutoBeltRefill: auto: watcher stopped (auto_refill off).");
		g_autoChainLive = false;
		g_autoPrevValid = false;
		g_autoArmLeft   = 0;
		g_autoTickCount = 0;
		for (uint32_t slot = 0; slot < MaxBeltSlots; ++slot) {
			g_autoArmed[slot] = false;
		}
		return;
	}

	// A running fill session moves bottles - every diff against it would be
	// self-induced noise. Drop the baseline and wait it out, but KEEP the
	// armed slots: bottles drunk while a pass was still landing used to be
	// discarded here and never re-triggered (the v0.34.0 double-tap bug).
	if (g_fillRunning) {
		g_autoPrevValid = false;
		reschedule();
		return;
	}

	if (++g_autoTickCount < g_config.autoPollFrames) {
		reschedule();
		return;
	}
	g_autoTickCount = 0;

	Snapshot snapshot {};
	if (!Gather(context, snapshot)) {
		// Not in a game (yet). The reschedule attempt fails outside games and
		// ends the chain; GameJoined restarts it.
		g_autoPrevValid = false;
		g_autoArmLeft   = 0;
		reschedule();
		return;
	}

	bool occupied[MaxBeltSlots] {};
	const int32_t capacity = static_cast<int32_t>(snapshot.beltRows) * BeltColumns;
	for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
		const int32_t slot = snapshot.belt[index].x;
		if (slot >= 0 && slot < capacity && slot < static_cast<int32_t>(MaxBeltSlots)) {
			occupied[slot] = true;
		}
	}

	// v0.35.6: teach the column memories on EVERY poll, not only at fill-pass
	// intros. A manual first-row swap made while the column is FULL used to be
	// invisible: no pass ran, so no diff ran, and by the time the next pass
	// started the new potion had usually been drunk already - the removal path
	// kept the old memory and the column refilled the wrong type (the 2026-09-04
	// 05:28/05:31 hp/mp swap report). Idempotent per snapshot and cheap (<=16x16
	// identity compares); same game thread as the pass intro, no new locks. Own
	// refills still never teach (they land codes the column already lists), and
	// configured/locked columns still ignore learning inside SetLearnedPolicy.
	UpdatePoliciesFromSnapshot(context, snapshot);

	if (g_autoCooldownLeft > 0) {
		--g_autoCooldownLeft;
	}

	if (g_autoArmLeft > 0) {
		// Debounce window: keep only the candidates that are STILL empty, add
		// fresh consumption, and trigger once when the window closes.
		--g_autoArmLeft;
		bool anyCandidate = false;
		for (int32_t slot = 0; slot < capacity; ++slot) {
			if (g_autoArmed[slot] && occupied[slot]) {
				g_autoArmed[slot] = false; // refilled (manually) in the meantime
			}
			if (g_autoPrevValid && g_autoPrev[slot] && !occupied[slot]) {
				g_autoArmed[slot] = true;  // consumed during the window
			}
			anyCandidate = anyCandidate || g_autoArmed[slot];
		}
		if (g_autoArmLeft > 0) {
			reschedule();
			return;
		}
		if (anyCandidate && g_autoCooldownLeft > 0) {
			// v0.34.1: still inside the previous pass's quiet time. Keep the
			// candidates and re-check next poll - they used to be dropped
			// here, which lost every drink taken within ~1 s of a pass.
			g_autoArmLeft = 1;
			reschedule();
			return;
		}
		if (anyCandidate) {
			int32_t firstCandidate = -1;
			int32_t candidateCount = 0;
			for (int32_t slot = 0; slot < capacity; ++slot) {
				if (g_autoArmed[slot]) {
					if (firstCandidate < 0) {
						firstCandidate = slot;
					}
					++candidateCount;
				}
			}
			Log(context,
				"AutoBeltRefill: auto: refill pass starting (slot %d, %d slot(s) empty).",
				firstCandidate,
				candidateCount);
			g_autoPrevValid    = false; // the pass will reshuffle the belt
			g_autoCooldownLeft = AutoCooldownPollsFor(g_config.autoPollFrames);
			g_autoRearmCount   = 0; // a fresh consumption resets the retry budget
			if (StartFillSession(context) != D2RL::Threads::Result::Success) {
				g_fillRunning = false;
			}
			reschedule();
			return;
		}
		// Window closed with nothing (left) to do.
		for (uint32_t slot = 0; slot < MaxBeltSlots; ++slot) {
			g_autoArmed[slot] = false;
		}
		g_autoRearmCount = 0; // clean close: full retry budget back
		g_autoPrevValid = true;
		std::memcpy(g_autoPrev, occupied, sizeof(g_autoPrev));
		reschedule();
		return;
	}

	// v0.34.1: detect consumption even during the cooldown. The baseline is
	// refreshed below either way - without this, anything drunk inside the
	// quiet time silently merged into the baseline and never triggered.
	if (g_autoPrevValid) {
		bool consumed = false;
		for (int32_t slot = 0; slot < capacity && !consumed; ++slot) {
			consumed = g_autoPrev[slot] && !occupied[slot];
		}
		if (consumed) {
			// Open the trailing window instead of firing immediately. Arm
			// ONLY the fresh slots: anything already armed stays armed.
			for (int32_t slot = 0; slot < capacity; ++slot) {
				if (!g_autoArmed[slot]) {
					g_autoArmed[slot] = g_autoPrev[slot] && !occupied[slot];
				}
			}
			g_autoArmLeft = static_cast<int32_t>(AutoDebouncePollsFor(g_config.autoPollFrames));
			reschedule();
			return;
		}
		// v0.35.0: any occupancy transition proves fresh activity - it
		// unlocks the state trigger below (which a 0-moved pass locks out).
		if (g_autoNeedsNewEvent) {
			for (int32_t slot = 0; slot < capacity; ++slot) {
				if (g_autoPrev[slot] != occupied[slot]) {
					g_autoNeedsNewEvent = false;
					break;
				}
			}
		}
	}

	// v0.35.0: state-based trigger - inventory stock waiting on empty
	// slots. Covers loot picked straight into the inventory (no belt
	// transition ever happens, so the consumption path cannot see it).
	// A pass that ended with 0 moved locks this until fresh activity.
	if (g_autoArmLeft == 0 && g_autoCooldownLeft == 0 && !g_autoNeedsNewEvent) {
		bool anyStock = false;
		for (int32_t slot = 0; slot < capacity && !anyStock; ++slot) {
			if (occupied[slot]) {
				continue;
			}
			const ColumnPolicy& policy = g_policy[BeltColumnOf(static_cast<uint32_t>(slot))];
			for (uint32_t index = 0; index < policy.count && !anyStock; ++index) {
				for (uint32_t item = 0; item < snapshot.inventoryCount; ++item) {
					if (snapshot.inventory[item].code == policy.codes[index]) {
						anyStock = true;
						break;
					}
				}
			}
		}
		if (anyStock) {
			// Arm every empty slot whose column memory lists an in-stock
			// code, then open the usual debounce window. The fill pass
			// itself does the exact per-column accounting.
			for (int32_t slot = 0; slot < capacity; ++slot) {
				bool match = false;
				if (!occupied[slot]) {
					const ColumnPolicy& policy = g_policy[BeltColumnOf(static_cast<uint32_t>(slot))];
					for (uint32_t index = 0; index < policy.count && !match; ++index) {
						for (uint32_t item = 0; item < snapshot.inventoryCount; ++item) {
							if (snapshot.inventory[item].code == policy.codes[index]) {
								match = true;
								break;
							}
						}
					}
				}
				g_autoArmed[slot] = match;
			}
			g_autoArmLeft = static_cast<int32_t>(AutoDebouncePollsFor(g_config.autoPollFrames));
			Log(context, "AutoBeltRefill: auto: inventory stock matches empty slot(s) - opening the refill window.");
			reschedule();
			return;
		}
	}

	// v0.35.1: first-row re-anchor trigger. When the player re-anchors a
	// column by placing a potion into its first row, a refill pass must run
	// immediately - the pass intro re-learns the column from that first row
	// and then fills the rest from the inventory. The stock trigger above
	// cannot see this: the stale column memory matches nothing in the
	// inventory, so a re-anchored empty column used to sit dormant until an
	// unrelated event opened a window (00:37:26 -> 00:40:36 in the
	// 2026-09-04 log). Goes quiet on its own: once the pass learns the
	// first-row code, it IS the column memory.
	if (g_autoArmLeft == 0 && g_autoCooldownLeft == 0 && !g_autoNeedsNewEvent) {
		bool reanchor = false;
		for (uint32_t column = 0; column < BeltColumns && !reanchor; ++column) {
			const ColumnPolicy& policy = g_policy[column];
			const int32_t       row0   = static_cast<int32_t>(column); // row 0 = slot == column
			if (!occupied[row0] || policy.locked) {
				continue;
			}
			bool hasEmpty = false;
			for (uint32_t row = 1; row < MaxRowsPerColumn; ++row) {
				const int32_t lower = static_cast<int32_t>(BeltSlotOf(row, column));
				hasEmpty = hasEmpty || (lower < capacity && !occupied[lower]);
			}
			if (!hasEmpty) {
				continue; // nothing to fill even after re-learning
			}
			uint32_t rowCode = 0;
			for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
				if (snapshot.belt[index].x == row0) {
					rowCode = snapshot.belt[index].code;
					break;
				}
			}
			bool known = rowCode == 0 || policy.count == 0;
			for (uint32_t index = 0; index < policy.count && !known; ++index) {
				known = policy.codes[index] == rowCode;
			}
			if (!known) {
				reanchor = true; // the first row no longer speaks for this memory
			}
		}
		if (reanchor) {
			for (int32_t slot = 0; slot < capacity; ++slot) {
				g_autoArmed[slot] = !occupied[slot];
			}
			g_autoArmLeft = static_cast<int32_t>(AutoDebouncePollsFor(g_config.autoPollFrames));
			Log(context, "AutoBeltRefill: auto: first-row anchor changed a column's type - opening the refill window.");
			reschedule();
			return;
		}
	}

	g_autoPrevValid = true;
	std::memcpy(g_autoPrev, occupied, sizeof(g_autoPrev));
	reschedule();
}

// Boots the monitor chain if it is not already running. Safe to call from
// any thread the loader calls us on (load, gameplay events, input actions,
// console commands).
void EnsureAutoWatch(const D2RL::PluginContext* context) noexcept {
	if (!g_config.autoRefill || g_threads == nullptr) {
		return;
	}
	if (g_autoChainLive) {
		// Liveness check: a dispatch accepted right before a game transition
		// can be silently dropped, leaving the flag stuck true with no chain
		// (02:49:44 - potions after a rejoin never refilled while F8 claimed
		// the watcher was running). No tick for 3 s = dead; restart it.
		const uint64_t age = GetTickCount64() - g_autoLastTickMs;
		if (age < AutoWatchStaleAfterMs) {
			return;
		}
		Log(context, "AutoBeltRefill: auto: watch chain stalled (no tick for %llu ms) - restarting it.",
			static_cast<unsigned long long>(age));
		g_autoChainLive = false;
	}
	if (Dispatch(context, RunAutoWatch) == D2RL::Threads::Result::Success) {
		g_autoChainLive  = true;
		g_autoLastTickMs = GetTickCount64();
		LogDebug(context, "AutoBeltRefill: auto: watch chain started (poll every %u frame(s)).", g_config.autoPollFrames);
	} else {
		// v0.34.4: a silent failure here left the toggle looking broken
		// after a restart (the 02:13 session). Say what happened.
		Log(context, "AutoBeltRefill: auto: could not start the watch chain right now - it retries on the next fill or game join.");
	}
}

// Gameplay events run on the UI thread; a game join is the moment the game
// thread (and therefore the dispatch service) becomes usable again.
void OnAutoGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
	if (event != nullptr && event->kind == D2RL::Lifecycle::GameplayEventKind::GameJoined) {
		// v0.34.5: a dispatch queued right before the previous game ended is
		// thrown away by the scene change while its "live" flag survives -
		// force-reset here so the fresh Dispatch below can always rebuild.
		// A genuinely live chain cannot survive a game transition anyway
		// (its Gather fails outside games and the chain ends itself).
		g_autoChainLive     = false;
		g_autoPrevValid     = false;
		g_autoArmLeft       = 0;
		g_autoCooldownLeft  = 0;
		g_autoTickCount     = 0;
		g_autoNeedsNewEvent = false;
		// v0.35.0: baseline fill right after joining. Without this the belt
		// stayed as it loaded until the FIRST consumption - empty slots the
		// player expects to be filled on entry waited for a potion drink.
		// The fill's intro snapshot also re-learns first-row types, so the
		// columns are ready before anything is drunk.
		if (!g_fillRunning) {
			if (StartFillSession(context) != D2RL::Threads::Result::Success) {
				g_fillRunning = false;
			}
		}
		EnsureAutoWatch(context);
	}
}

// ---------------------------------------------------------------------- scan

void RunScan(const D2RL::PluginContext* context, void*) noexcept {
	Snapshot snapshot {};
	if (!Gather(context, snapshot)) {
		return;
	}

	DumpBelt(context, snapshot);
	DumpInventory(context, snapshot);
	DumpEquipment(context, snapshot);

	if (snapshot.dropped != 0) {
		Log(context, "AutoBeltRefill: %u item(s) did not fit in the scan buffers.", snapshot.dropped);
	}

	const uint32_t rows = snapshot.beltRows;
	for (uint32_t column = 0; column < BeltColumns; ++column) {
		const uint32_t targetCode = ColumnTargetCode(snapshot, column, rows);
		if (targetCode == 0) {
			Log(context, "AutoBeltRefill: column %u is empty.", column);
			continue;
		}
		char text[5] {};
		CodeText(targetCode, text);
		Log(context, "AutoBeltRefill: column %u target code = %s.", column, text);
	}

	for (uint32_t column = 0; column < BeltColumns; ++column) {
		char description[96] {};
		DescribeCodeList(g_policy[column], description);
		LogDebug(context, "AutoBeltRefill: policy: column %u = %s.", column, description);
	}
}

// ------------------------------------------------------------------ policy io

// Shows the per-column memories next to the current belt contents.
void RunPolicy(const D2RL::PluginContext* context, void*) noexcept {
	Snapshot snapshot {};
	const bool gathered = Gather(context, snapshot);
	// Replay the snapshot diff right here too - on a fresh session a bare
	// "belt-policy" reads the first-row codes instead of leaving the
	// columns at "unassigned" until the next fill key.
	if (gathered) {
		UpdatePoliciesFromSnapshot(context, snapshot);
	}
	for (uint32_t column = 0; column < BeltColumns; ++column) {
		char description[96] {};
		DescribeCodeList(g_policy[column], description);

		char     content[80] {};
		uint32_t used = 0;
		if (gathered) {
			for (uint32_t row = 0; row < snapshot.beltRows && used + 6 < sizeof(content); ++row) {
				const Cell* cell = FindSlot(snapshot, BeltSlotOf(row, column));
				if (cell != nullptr && cell->code != 0) {
					char text[5] {};
					CodeText(cell->code, text);
					const int written = std::snprintf(content + used, sizeof(content) - used, "%s ", text);
					if (written > 0) {
						used += static_cast<uint32_t>(written);
					}
				} else {
					content[used++] = '.';
					content[used++] = ' ';
				}
			}
		}
		Log(context,
			"AutoBeltRefill: policy: column %u = %s%s | belt bottom-up: %s",
			column,
			description,
			g_policy[column].locked ? " [locked]" : "",
			gathered ? content : "(no game)");
	}
}

// Re-records every column from its FIRST row. An empty first row leaves the
// column's memory untouched (see snapshot_key in the config file).
void RunSnapshot(const D2RL::PluginContext* context, void*) noexcept {
	Snapshot snapshot {};
	if (!Gather(context, snapshot)) {
		return;
	}
	// v0.34.5: the F8 snapshot doubles as a watcher health check - it shows
	// whether auto_refill is on and whether the watch chain is ticking.
	Log(context,
		"AutoBeltRefill: snapshot: auto_refill is %s (watcher %s).",
		g_config.autoRefill ? "ON" : "OFF",
		g_autoChainLive ? "running" : "stopped");
	for (uint32_t column = 0; column < BeltColumns; ++column) {
		const Cell* cell = FindSlot(snapshot, column); // slot == column == row 0
		if (cell != nullptr && cell->code != 0 && FamilyOf(cell->code) != Family::None) {
			SetLearnedPolicy(column, cell->code, "snapshot key");
		} else {
			char description[96] {};
			DescribeCodeList(g_policy[column], description);
			Log(context,
				"AutoBeltRefill: snapshot: column %u first row empty - keeps %s.",
				column,
				description);
		}
	}
}

// Read-only dump of the parsed config (belt-config).
void RunConfig(const D2RL::PluginContext* context, void*) noexcept {
	char fillName[16] {};
	char snapName[16] {};
	KeyName(g_config.fillKey, fillName);
	KeyName(g_config.snapshotKey, snapName);
	Log(context,
		"AutoBeltRefill: config: enabled=%s fill_key=%s snapshot_key=%s prefer_large=%s auto_refill=%s%s auto_poll_frames=%u.",
		g_config.enabled ? "true" : "false",
		fillName,
		snapName,
		g_config.preferLarge ? "true" : "false",
		g_config.autoRefill ? "true" : "false",
		g_config.autoRefill ? " (consumption-triggered refill passes)" : "",
		g_config.autoPollFrames);

	for (uint32_t column = 0; column < BeltColumns; ++column) {
		const ColumnConfig& config = g_config.column[column];
		char list[96] {};
		if (config.count == 0) {
			std::snprintf(list, sizeof(list), "(not specified)");
		} else {
			uint32_t used = 0;
			for (uint32_t index = 0; index < config.count && used + 8 < sizeof(list); ++index) {
				char text[5] {};
				CodeText(config.codes[index], text);
				const int written = std::snprintf(list + used, sizeof(list) - used, index == 0 ? "%s" : " > %s", text);
				if (written <= 0 || used + static_cast<uint32_t>(written) >= sizeof(list)) {
					break;
				}
				used += static_cast<uint32_t>(written);
			}
		}
		Log(context,
			"AutoBeltRefill: config: column %u consumables=%s locked=%s.",
			column + 1,
			list,
			config.locked ? "true" : "false");
	}
}

// Forgets the learned memories; configured columns come straight back from
// the file, the others re-record on the next fill.
void RunReset(const D2RL::PluginContext* context, void*) noexcept {
	for (uint32_t column = 0; column < BeltColumns; ++column) {
		g_policy[column] = ColumnPolicy {};
	}
	ApplyConfiguredPolicies();
	g_lastBeltCount = 0;
	g_lastBeltValid = false;
	Log(context, "AutoBeltRefill: policy: learned memories reset (configured columns restored).");
}

// ------------------------------------------------------------------ belt-place
//
// The v0.33.2 experiment, kept in v0.33.5 for diagnostics only: move ONE
// inventory item into the belt through the native BeltTransfer using the
// plain shift-click convention (a3 = 0, a4 = 1, out zeroed) - the game's
// own search picks the slot ("auto" mode). A per-frame verifier reports
// whether the item really left the inventory page and which slot(s) it
// landed in, by diffing the belt occupancy.
//
// History: v0.33.3/v0.33.4 added slot/drag/raw/steer modes to probe
// explicit-slot placement. All were disproved by live testing (the
// placement half resolves the item from the cursor page; the server only
// honours its own slot choice - see the handover doc §4/§5) and were
// removed in v0.33.5. The command never writes the column memories
// itself; a row-0 placement still re-records its column through the
// ordinary snapshot diff on the next fill/policy read.

int32_t   g_placeSlot    = -1;
uint32_t  g_placeCode    = 0;
bool      g_placeHasCode = false;

// Landing verification for the experiment: the item pointer, how many
// frames we have waited, a short "code" label for the report, the wanted
// slot and the pre-call belt occupancy, so the verifier can name the
// landing slot(s).
void*    g_placeVerifyItem  = nullptr;
uint32_t g_placeVerifyFrames = 0;
char     g_placeVerifyText[24] = {};
int32_t  g_placeVerifyWant     = -1;
bool     g_placeVerifyPrev[MaxBeltSlots] = {};

constexpr uint32_t PlaceVerifyMaxFrames = 150;

void RunPlaceVerify(const D2RL::PluginContext* context, void*) noexcept;

// Formats one line, writes it to the log AND to the in-game console so
// the experiment can be read without leaving the game.
void PlaceReport(const D2RL::PluginContext* context, bool error, const char* format, ...) noexcept {
	if (context == nullptr) {
		return;
	}
	char message[256] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	Log(context, "%s", message);
	context->WriteConsoleMessage(message, error ? D2RL::ConsoleMessageKind::Error : D2RL::ConsoleMessageKind::Output);
}

void RunBeltPlace(const D2RL::PluginContext* context, void*) noexcept {
	const int32_t  wantedSlot = g_placeSlot;
	const uint32_t wantedCode = g_placeCode;
	const bool     hasCode    = g_placeHasCode;
	g_placeSlot = -1;

	if (context == nullptr || !ResolveNatives(context)) {
		PlaceReport(context, true, "AutoBeltRefill: place: natives not resolved.");
		return;
	}
	if (OriginalBeltTransfer == nullptr) {
		PlaceReport(context, true, "AutoBeltRefill: place: BeltTransfer hook not installed.");
		return;
	}
	if (wantedSlot < 0 || wantedSlot >= static_cast<int32_t>(MaxBeltSlots)) {
		PlaceReport(context, true, "AutoBeltRefill: place: slot %d out of range (0..%u).", wantedSlot, MaxBeltSlots - 1);
		return;
	}
	if (g_fillRunning) {
		PlaceReport(context, true, "AutoBeltRefill: place: a fill session is in progress - try again once it finishes.");
		return;
	}
	if (g_placeVerifyItem != nullptr) {
		PlaceReport(context, true, "AutoBeltRefill: place: a previous belt-place is still being verified - wait a moment.");
		return;
	}

	void* player    = nullptr;
	void* inventory = nullptr;
	__try {
		player = GetLocalPlayer(GetLocalDataContext());
		if (player != nullptr) {
			inventory = GetUnitInventory(player);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		inventory = nullptr;
	}
	if (player == nullptr || inventory == nullptr) {
		PlaceReport(context, true, "AutoBeltRefill: place: no player or inventory.");
		return;
	}

	// The target slot must be free and within the equipped belt's rows.
	Snapshot snapshot {};
	if (!Gather(context, snapshot)) {
		return;
	}
	if (static_cast<uint32_t>(wantedSlot) >= BeltColumns * snapshot.beltRows) {
		PlaceReport(context,
			true,
			"AutoBeltRefill: place: slot %d is beyond the belt (%u row(s) equipped).",
			wantedSlot,
			snapshot.beltRows);
		return;
	}
	if (FindSlot(snapshot, static_cast<uint32_t>(wantedSlot)) != nullptr) {
		bool occupied[MaxBeltSlots] {};
		for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
			const int32_t slot = snapshot.belt[index].x;
			if (slot >= 0 && slot < static_cast<int32_t>(MaxBeltSlots)) {
				occupied[slot] = true;
			}
		}
		char freeSlots[64] = "(none)";
		uint32_t written   = 0;
		freeSlots[0]       = '\0';
		const int32_t capacity = static_cast<int32_t>(snapshot.beltRows) * BeltColumns;
		for (int32_t slot = 0; slot < capacity; ++slot) {
			if (!occupied[slot]) {
				const int32_t count = std::snprintf(freeSlots + written,
					sizeof(freeSlots) - written,
					"%s%d",
					written == 0 ? "" : ", ",
					slot);
				if (count <= 0 || static_cast<uint32_t>(count) >= sizeof(freeSlots) - written) {
					break;
				}
				written += static_cast<uint32_t>(count);
			}
		}
		if (written == 0) {
			std::snprintf(freeSlots, sizeof(freeSlots), "(none)");
		}
		PlaceReport(context,
			true,
			"AutoBeltRefill: place: slot %d is occupied - free slots: %s.",
			wantedSlot,
			freeSlots);
		return;
	}

	// Pick the item: the given code when specified, otherwise the first
	// inventory item any column asks for.
	void*    item = nullptr;
	uint32_t code = 0;
	__try {
		for (void* scan = GetFirstItem(inventory); scan != nullptr; scan = GetNextItem(scan)) {
			if (GetUnitType(scan) != ItemUnitType) {
				continue;
			}
			const auto* data = static_cast<const uint8_t*>(GetItemData(scan));
			if (data == nullptr || data[ItemDataPageOffset] != 0) {
				continue;
			}
			const uint32_t scanCode = GetItemCode(scan);
			if (scanCode == 0) {
				continue;
			}
			if (hasCode) {
				if (scanCode == wantedCode) {
					item = scan;
					code = scanCode;
					break;
				}
				continue;
			}
			bool wanted = false;
			for (uint32_t column = 0; column < BeltColumns && !wanted; ++column) {
				const ColumnPolicy& policy = g_policy[column];
				for (uint32_t index = 0; index < policy.count; ++index) {
					if (policy.codes[index] == scanCode) {
						wanted = true;
						break;
					}
				}
			}
			if (wanted) {
				item = scan;
				code = scanCode;
				break;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		item = nullptr;
	}
	if (item == nullptr) {
		PlaceReport(context, true, "AutoBeltRefill: place: no matching item on the inventory page.");
		return;
	}

	char codeText[5] {};
	CodeText(code, codeText);

	// Auto convention: the plain shift-click parameters, out zeroed - the
	// game's own search picks the slot. (All explicit-slot conventions
	// were disproved and removed; see the section comment.)
	uint8_t out[32] {};

	bool moved = false;
	bool threw = false;
	g_fillPending = item; // the hook must not learn from our own transfer
	__try {
		moved = OriginalBeltTransfer(item, player, 0, 1, out);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		moved = false;
		threw = true;
	}
	g_fillPending = nullptr;

	int32_t landed = 0;
	uint8_t flag   = 0;
	std::memcpy(&landed, out, sizeof(landed));
	std::memcpy(&flag, out + 4, sizeof(flag));

	PlaceReport(context,
		false,
		"AutoBeltRefill: place: %s -> slot %d (auto: a3=0 a4=1 out=0): %s (landed %d, flag %u).",
		codeText,
		wantedSlot,
		threw ? "EXCEPTION" : (moved ? "requested" : "refused"),
		landed,
		static_cast<uint32_t>(flag));
	if (moved) {
		// Watch the item across frames: the request being accepted says
		// nothing (the drag convention proved that), only the item really
		// leaving the inventory page does. Remember the pre-move
		// occupancy so the verifier can name the landing slot(s).
		bool occupiedNow[MaxBeltSlots] {};
		for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
			const int32_t slot = snapshot.belt[index].x;
			if (slot >= 0 && slot < static_cast<int32_t>(MaxBeltSlots)) {
				occupiedNow[slot] = true;
			}
		}
		for (uint32_t slot = 0; slot < MaxBeltSlots; ++slot) {
			g_placeVerifyPrev[slot] = occupiedNow[slot];
		}
		g_placeVerifyWant = wantedSlot;
		std::snprintf(g_placeVerifyText, sizeof(g_placeVerifyText), "%s auto", codeText);
		g_placeVerifyItem   = item;
		g_placeVerifyFrames = 0;
		Dispatch(context, RunPlaceVerify);
	}
}

// Landing verifier for belt-place: re-dispatches onto later frames until the
// item leaves the inventory page (or the frame budget runs out). Runs on
// the game thread. Also diffs the belt occupancy against the pre-call
// snapshot, so the report names the slot(s) the bottle really landed in -
// the difference between "the game honoured the request" and "it chose
// another slot by itself".
void RunPlaceVerify(const D2RL::PluginContext* context, void*) noexcept {
	void* item = g_placeVerifyItem;
	if (context == nullptr || item == nullptr) {
		g_placeVerifyItem = nullptr;
		return;
	}

	bool landed = false;
	__try {
		const auto* data = static_cast<const uint8_t*>(GetItemData(item));
		landed = (data == nullptr) || (data[ItemDataPageOffset] != 0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		landed = false;
	}

	if (landed) {
		// Which belt slots are newly occupied compared to before the move?
		char     slots[48] {};
		uint32_t written  = 0;
		bool     inWanted = false;
		Snapshot snapshot {};
		if (Gather(context, snapshot)) {
			bool now[MaxBeltSlots] {};
			for (uint32_t index = 0; index < snapshot.beltCount; ++index) {
				const int32_t slot = snapshot.belt[index].x;
				if (slot >= 0 && slot < static_cast<int32_t>(MaxBeltSlots)) {
					now[slot] = true;
				}
			}
			for (uint32_t slot = 0; slot < MaxBeltSlots; ++slot) {
				if (now[slot] && !g_placeVerifyPrev[slot]) {
					if (static_cast<int32_t>(slot) == g_placeVerifyWant) {
						inWanted = true;
					}
					const int32_t count = std::snprintf(slots + written,
						sizeof(slots) - written,
						"%s%u",
						written == 0 ? "" : ",",
						slot);
					if (count <= 0 || static_cast<uint32_t>(count) >= sizeof(slots) - written) {
						break;
					}
					written += static_cast<uint32_t>(count);
				}
			}
		}
		if (written == 0) {
			std::snprintf(slots, sizeof(slots), "?");
		}

		const char* verdict = inWanted ? "" : " - the natural path chose a different slot";
		Log(context,
			"AutoBeltRefill: place: verify: %s landed after %u frame(s) at slot(s) %s (wanted %d)%s.",
			g_placeVerifyText,
			g_placeVerifyFrames,
			slots,
			g_placeVerifyWant,
			verdict);
		PlaceReport(context,
			false,
			"AutoBeltRefill: belt-place: %s landed after %u frame(s) at slot(s) %s (wanted %d)%s.",
			g_placeVerifyText,
			g_placeVerifyFrames,
			slots,
			g_placeVerifyWant,
			verdict);
		g_placeVerifyItem = nullptr;
		return;
	}

	++g_placeVerifyFrames;
	if (g_placeVerifyFrames > PlaceVerifyMaxFrames) {
		Log(context,
			"AutoBeltRefill: place: verify: %s never moved after %u frame(s) - this calling convention does not move inventory items.",
			g_placeVerifyText,
			PlaceVerifyMaxFrames);
		PlaceReport(context,
			true,
			"AutoBeltRefill: belt-place: %s never moved (%u frame(s)) - this convention does not move inventory items.",
			g_placeVerifyText,
			PlaceVerifyMaxFrames);
		g_placeVerifyItem = nullptr;
		return;
	}
	Dispatch(context, RunPlaceVerify);
}

// -------------------------------------------------------------------- commands

auto Dispatch(const D2RL::PluginContext* context, D2RL::Threads::Callback callback) noexcept -> D2RL::Threads::Result {
	if (g_threads == nullptr) {
		return D2RL::Threads::Result::Unavailable;
	}
	return g_threads->runOnGameThread(context, callback, nullptr);
}

auto ConsoleScan(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	const auto queued = Dispatch(command->plugin, RunScan);
	if (queued != D2RL::Threads::Result::Success) {
		command->plugin->WriteConsoleError("AutoBeltRefill: no game thread. Enter a local or TCP/IP host game first.");
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

// Starts a fresh fill session: the routine re-dispatches itself across frames
// until the belt is full (or nothing else fits), so one key press is enough.
auto ConsoleFill(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	const auto queued = StartFillSession(command->plugin);
	if (queued != D2RL::Threads::Result::Success) {
		g_fillRunning = false;
		command->plugin->WriteConsoleError("AutoBeltRefill: no game thread. Enter a local or TCP/IP host game first.");
		return D2RL::ConsoleCommandResult::Failed;
	}
	EnsureAutoWatch(command->plugin);
	return D2RL::ConsoleCommandResult::Handled;
}

// belt-place <slot 0-15> [code]
auto ConsoleBeltPlace(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	const char*    args       = command->args;
	const uint32_t argsLength = command->argsLength;

	// Split into at most three blank-separated tokens (length-bounded - the
	// SDK does not guarantee a terminator).
	const char* tokens[3] {};
	uint32_t    tokenLens[3] {};
	uint32_t    tokenCount = 0;
	uint32_t    cursor     = 0;
	while (cursor < argsLength && tokenCount < 3) {
		while (cursor < argsLength && IsBlankChar(args[cursor])) {
			++cursor;
		}
		if (cursor >= argsLength) {
			break;
		}
		const uint32_t start = cursor;
		while (cursor < argsLength && !IsBlankChar(args[cursor])) {
			++cursor;
		}
		tokens[tokenCount]   = args + start;
		tokenLens[tokenCount] = cursor - start;
		++tokenCount;
	}
	if (tokenCount == 0) {
		command->plugin->WriteConsoleError("AutoBeltRefill: usage: belt-place <slot 0-15> [code]");
		return D2RL::ConsoleCommandResult::Failed;
	}

	int32_t slot    = 0;
	bool    slotOk  = tokenLens[0] >= 1 && tokenLens[0] <= 2;
	for (uint32_t index = 0; index < tokenLens[0] && slotOk; ++index) {
		const char c = tokens[0][index];
		if (c < '0' || c > '9') {
			slotOk = false;
			break;
		}
		slot = slot * 10 + (c - '0');
	}
	if (!slotOk || slot >= static_cast<int32_t>(MaxBeltSlots)) {
		command->plugin->WriteConsoleError("AutoBeltRefill: belt-place: slot must be 0..15 (slot = row * 4 + column; the log prints both).");
		return D2RL::ConsoleCommandResult::Failed;
	}

	// Optional item code: one to four printable characters, padded with
	// spaces to the game's four-byte little-endian encoding.
	uint32_t code    = 0;
	bool     hasCode = false;
	if (tokenCount >= 2) {
		if (tokenLens[1] < 1 || tokenLens[1] > 4) {
			command->plugin->WriteConsoleError("AutoBeltRefill: belt-place: code must be 1..4 characters (e.g. hp5, rvl, tsc).");
			return D2RL::ConsoleCommandResult::Failed;
		}
		bool codeOk = true;
		for (uint32_t index = 0; index < tokenLens[1]; ++index) {
			const char c = tokens[1][index];
			if (c < 0x21 || c > 0x7E) {
				codeOk = false;
				break;
			}
			code |= static_cast<uint32_t>(static_cast<unsigned char>(c)) << (index * 8U);
		}
		if (!codeOk) {
			command->plugin->WriteConsoleError("AutoBeltRefill: belt-place: code must be printable ASCII.");
			return D2RL::ConsoleCommandResult::Failed;
		}
		for (uint32_t index = tokenLens[1]; index < 4; ++index) {
			code |= 0x20U << (index * 8U);
		}
		hasCode = true;
	}

	// Note: the explicit-slot modes (steer/slot/drag/raw) were disproved
	// and removed in v0.33.5; only the game's own slot choice remains.

	g_placeSlot    = slot;
	g_placeCode    = code;
	g_placeHasCode = hasCode;

	const auto queued = Dispatch(command->plugin, RunBeltPlace);
	if (queued != D2RL::Threads::Result::Success) {
		g_placeSlot = -1;
		command->plugin->WriteConsoleError("AutoBeltRefill: no game thread. Enter a local or TCP/IP host game first.");
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

auto ConsoleWatch(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	const auto queued = Dispatch(command->plugin, RunWatch);
	if (queued != D2RL::Threads::Result::Success) {
		command->plugin->WriteConsoleError("AutoBeltRefill: no game thread. Enter a local or TCP/IP host game first.");
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

auto ConsolePolicy(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	const auto queued = Dispatch(command->plugin, RunPolicy);
	if (queued != D2RL::Threads::Result::Success) {
		command->plugin->WriteConsoleError("AutoBeltRefill: no game thread. Enter a local or TCP/IP host game first.");
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

auto ConsoleReset(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	const auto queued = Dispatch(command->plugin, RunReset);
	if (queued != D2RL::Threads::Result::Success) {
		command->plugin->WriteConsoleError("AutoBeltRefill: no game thread. Enter a local or TCP/IP host game first.");
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

auto ConsoleSnapshot(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	const auto queued = Dispatch(command->plugin, RunSnapshot);
	if (queued != D2RL::Threads::Result::Success) {
		command->plugin->WriteConsoleError("AutoBeltRefill: no game thread. Enter a local or TCP/IP host game first.");
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

auto ConsoleConfig(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	// Pure config dump - no game state involved, no dispatch needed.
	RunConfig(command->plugin, nullptr);
	return D2RL::ConsoleCommandResult::Handled;
}

auto ConsoleVerify(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	struct RvaProbe {
		const char*    name;
		uint64_t       rva;
		const uint8_t* expected;
		uint32_t       size;
	};
	static const RvaProbe probes[4] = {
		{ "GetFreeBeltSlot", GetFreeBeltSlotRva, GetFreeBeltSlotBytes, sizeof(GetFreeBeltSlotBytes) },
		{ "CanPutInBelt",    CanPutInBeltRva,    CanPutInBeltBytes,    sizeof(CanPutInBeltBytes) },
		{ "BeltTransfer",    BeltTransferRva,    BeltTransferBytes,    sizeof(BeltTransferBytes) },
		{ "Pickup",          PickupRva,          PickupBytes,          sizeof(PickupBytes) },
	};

	for (const RvaProbe& probe : probes) {
		const auto ok = command->plugin->CheckExpectedBytes(probe.rva, probe.expected, probe.size);
		Log(command->plugin,
			"AutoBeltRefill: verify: %-16s rva=0x%llX -> %s",
			probe.name,
			static_cast<unsigned long long>(probe.rva),
			ok ? "MATCH (address is correct)" : "bytes differ (wrong build or address)");
	}
	return D2RL::ConsoleCommandResult::Handled;
}

// ----------------------------------------------------------------- key binding

auto OnFillKey(const D2RL::PluginContext* context, const D2RL::Input::ActionEvent* event, void*) noexcept -> D2RL::Input::ActionResult {
	if (context == nullptr || event == nullptr || event->kind != D2RL::Input::ActionEventKind::Pressed) {
		return D2RL::Input::ActionResult::Ignored;
	}

	if (StartFillSession(context) != D2RL::Threads::Result::Success) {
		g_fillRunning = false;
	}
	EnsureAutoWatch(context);
	return D2RL::Input::ActionResult::Handled;
}

// The snapshot key: re-record every column from its first row. Locked
// columns ignore it (SetLearnedPolicy early-returns).
auto OnSnapshotKey(const D2RL::PluginContext* context, const D2RL::Input::ActionEvent* event, void*) noexcept -> D2RL::Input::ActionResult {
	if (context == nullptr || event == nullptr || event->kind != D2RL::Input::ActionEventKind::Pressed) {
		return D2RL::Input::ActionResult::Ignored;
	}

	Dispatch(context, RunSnapshot);
	return D2RL::Input::ActionResult::Handled;
}

// ------------------------------------------------------------------ hook setup

auto InstallBeltHook(const D2RL::PluginContext* context) noexcept -> bool {
	void* original = nullptr;
	auto  registration = D2RL::MakeInlineHook(
		 GetFreeBeltSlotRva,
		 GetFreeBeltSlotBytes,
		 static_cast<uint32_t>(sizeof(GetFreeBeltSlotBytes)),
		 reinterpret_cast<void*>(&HookGetFreeBeltSlot),
		 &original);

	if (!context->InstallInlineHook(registration)) {
		Log(context, "AutoBeltRefill: hook: GetFreeBeltSlot inline hook FAILED.");
		return false;
	}

	OriginalGetFreeBeltSlot = reinterpret_cast<GetFreeBeltSlotFn>(original);
	g_moduleBase            = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	g_hookLogContext        = context;
	GetItemCode             = reinterpret_cast<GetItemCodeFn>(g_moduleBase + GetItemCodeRva);

	// Observation probe on the belt-availability test: the automatic mode
	// will need to know when the game itself asks "can this go in the belt?".
	{
		void* canOriginal = nullptr;
		auto  canRegistration = D2RL::MakeInlineHook(
			 CanPutInBeltRva,
			 CanPutInBeltBytes,
			 static_cast<uint32_t>(sizeof(CanPutInBeltBytes)),
			 reinterpret_cast<void*>(&HookCanPutInBelt),
			 &canOriginal);
		if (context->InstallInlineHook(canRegistration)) {
			OriginalCanPutInBelt = reinterpret_cast<CanPutInBeltFn>(canOriginal);
			LogDebug(context, "AutoBeltRefill: hook: CanPutInBelt probe installed at 0x%llX.", static_cast<unsigned long long>(CanPutInBeltRva));
		} else {
			Log(context, "AutoBeltRefill: hook: CanPutInBelt probe NOT installed (bytes differ).");
		}
	}

	// Observation hook on the auto-pickup routine.
	{
		void* pickOriginal = nullptr;
		auto  pickRegistration = D2RL::MakeInlineHook(
			 PickupRva,
			 PickupBytes,
			 static_cast<uint32_t>(sizeof(PickupBytes)),
			 reinterpret_cast<void*>(&HookPickup),
			 &pickOriginal);
		if (context->InstallInlineHook(pickRegistration)) {
			OriginalPickup = reinterpret_cast<PickupFn>(pickOriginal);
			LogDebug(context, "AutoBeltRefill: hook: Pickup logger installed at 0x%llX.", static_cast<unsigned long long>(PickupRva));
		} else {
			Log(context, "AutoBeltRefill: hook: Pickup logger NOT installed (bytes differ).");
		}
	}

	// Hook on the belt-transfer routine we drive. Logging what the game hands
	// it during a real shift-click documents the calling convention for the
	// automatic mode.
	{
		void* beltOriginal = nullptr;
		auto  beltRegistration = D2RL::MakeInlineHook(
			 BeltTransferRva,
			 BeltTransferBytes,
			 static_cast<uint32_t>(sizeof(BeltTransferBytes)),
			 reinterpret_cast<void*>(&HookBeltTransfer),
			 &beltOriginal);
		if (context->InstallInlineHook(beltRegistration)) {
			OriginalBeltTransfer = reinterpret_cast<BeltTransferFn>(beltOriginal);
			LogDebug(context, "AutoBeltRefill: hook: BeltTransfer logger installed at 0x%llX.", static_cast<unsigned long long>(BeltTransferRva));
		} else {
			Log(context, "AutoBeltRefill: hook: BeltTransfer logger NOT installed (bytes differ).");
		}
	}
	LogDebug(context,
		"AutoBeltRefill: hook: GetFreeBeltSlot installed (original=%p, module base=0x%llX).",
		reinterpret_cast<void*>(OriginalGetFreeBeltSlot),
		static_cast<unsigned long long>(g_moduleBase));
	return true;
}

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &AutoBeltRefillInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	LoadConfig(context);
	g_debugLogs = g_config.debugLogs; // hooks run before this on no thread; safe to sync once here
	if (!g_config.enabled) {
		context->LogInfo("AutoBeltRefill: disabled by config (enabled = false) - nothing registered.");
		return true;
	}

	if (context->QueryService(D2RL::ServiceId::Inventory, D2RL::InventoryServiceV1Version, &g_inventory) != D2RL::ServiceQueryResult::Success
	    || !D2RL::HasInventoryServiceV1Field(g_inventory, D2RL::InventoryServiceV1RequiredSize)) {
		return false;
	}

	if (context->QueryService(D2RL::ServiceId::Thread, D2RL::ThreadServiceV1Version, &g_threads) != D2RL::ServiceQueryResult::Success
	    || !D2RL::HasThreadServiceV1Field(g_threads, D2RL::ThreadServiceV1RequiredSize)) {
		return false;
	}

	if (!context->RegisterConsoleCommand("belt-fill", ConsoleFill, "Refill every empty belt slot from the inventory per the column memories.")) {
		return false;
	}

	if (!context->RegisterConsoleCommand("belt-place", ConsoleBeltPlace, "Diagnostics: move one inventory item into the belt via the game's own slot search (usage: belt-place <slot 0-15> [code]).")) {
		return false;
	}

	if (!context->RegisterConsoleCommand("belt-policy", ConsolePolicy, "Show the per-column memory next to the current belt contents.")) {
		return false;
	}

	if (!context->RegisterConsoleCommand("belt-config", ConsoleConfig, "Show the parsed config file.")) {
		return false;
	}

	if (!context->RegisterConsoleCommand("belt-snapshot", ConsoleSnapshot, "Re-record every column from its first belt row.")) {
		return false;
	}

	if (!context->RegisterConsoleCommand("belt-reset", ConsoleReset, "Forget the learned memories; the next fill re-records them.")) {
		return false;
	}

	if (!context->RegisterConsoleCommand("belt-scan", ConsoleScan, "Dump the belt and inventory layout to the log.")) {
		return false;
	}

	if (!context->RegisterConsoleCommand("belt-watch", ConsoleWatch, "List every distinct caller of GetFreeBeltSlot and any Pickup activity.")) {
		return false;
	}

	if (!context->RegisterConsoleCommand("belt-verify", ConsoleVerify, "Read-only: check whether the native belt-hook addresses match this build.")) {
		return false;
	}

	if (context->QueryService(D2RL::ServiceId::Input, D2RL::InputServiceV1Version, &g_input) == D2RL::ServiceQueryResult::Success
	    && D2RL::HasInputServiceV1Field(g_input, D2RL::InputServiceV1RequiredSize)) {
		// v0.35.6: the actions are ALWAYS registered. fill_key = "" (or a
		// missing key) registers them with no default binding - they show up
		// as "无" in the game's control settings under the Auto Belt Refill
		// category, where the player can bind them directly. Nothing fires
		// until a key is bound, so mod players with a full F-row stay safe
		// (the v0.35.3 guarantee) while gaining the in-game binding option.
		// The console command belt-fill keeps working either way.
		const D2RL::Input::ActionRegistration fillAction {
			.structSize      = D2RL::Input::ActionRegistrationSize,
			.logicalId       = "autobeltrefill.fill",
			.displayName     = "Refill Belt",
			.category        = "Auto Belt Refill",
			.defaultPrimary  = { g_config.fillKey, D2RL::Input::Modifier::None },
			.callback        = OnFillKey,
		};
		D2RL::Input::ActionHandle fillHandle = D2RL::Input::InvalidHandle;
		if (g_input->registerAction(context, &fillAction, &fillHandle) != D2RL::Input::Result::Success) {
			g_input = nullptr;
		} else {
			// v0.35.7: read back what the game's control settings actually
			// hold for this action (a previously in-game binding beats the
			// toml default). Failure is fine - the banner falls back to the
			// toml value.
			D2RL::Input::Binding live {};
			if (g_input->getBinding(context, fillHandle, D2RL::Input::BindingSlot::Primary, &live)
				== D2RL::Input::Result::Success) {
				g_liveFillKey    = live.key;
				g_liveFillValid  = true;
			}
		}
	} else {
		g_input = nullptr;
	}

	if (g_input != nullptr) {
		const D2RL::Input::ActionRegistration snapshotAction {
			.structSize      = D2RL::Input::ActionRegistrationSize,
			.logicalId       = "autobeltrefill.snapshot",
			.displayName     = "Snapshot Belt Columns",
			.category        = "Auto Belt Refill",
			.defaultPrimary  = { g_config.snapshotKey, D2RL::Input::Modifier::None },
			.callback        = OnSnapshotKey,
		};
		D2RL::Input::ActionHandle snapshotHandle = D2RL::Input::InvalidHandle;
		if (g_input->registerAction(context, &snapshotAction, &snapshotHandle) != D2RL::Input::Result::Success) {
			Log(context, "AutoBeltRefill: snapshot action registration failed - belt-snapshot still works.");
		} else {
			D2RL::Input::Binding live {};
			if (g_input->getBinding(context, snapshotHandle, D2RL::Input::BindingSlot::Primary, &live)
				== D2RL::Input::Result::Success) {
				g_liveSnapshotKey    = live.key;
				g_liveSnapshotValid  = true;
			}
		}
	}

	// Configured columns are live from the start; UpdatePoliciesFromSnapshot
	// re-applies them after every player change as well.
	ApplyConfiguredPolicies();

	// Only install the hooks after the byte patterns have been shown to match
	// this build. InstallInlineHook re-checks the same bytes, so a mismatch
	// fails safely instead of patching the wrong address.
	InstallBeltHook(context);

	char banner[512] {};
	// v0.35.7: the banner reports the LIVE bindings (game control settings)
	// when the read-back succeeded - "toml empty but F9 bound in game" must
	// not print as "no hotkeys bound". Falls back to the toml values.
	const D2RL::Input::Key effFillKey = g_liveFillValid ? g_liveFillKey : g_config.fillKey;
	const D2RL::Input::Key effSnapKey = g_liveSnapshotValid ? g_liveSnapshotKey : g_config.snapshotKey;
	char fillName[16] {};
	char snapName[16] {};
	KeyName(effFillKey, fillName);
	KeyName(effSnapKey, snapName);
	// v0.35.3: say what is actually bound - unbound keys must not read like
	// a broken binding ("unbound = fill belt").
	char keyHint[128] {};
	if (effFillKey != D2RL::Input::Key::None && effSnapKey != D2RL::Input::Key::None) {
		std::snprintf(keyHint, sizeof(keyHint), "%s = fill belt, %s = snapshot columns.", fillName, snapName);
	} else if (effFillKey != D2RL::Input::Key::None) {
		std::snprintf(keyHint, sizeof(keyHint), "%s = fill belt (no snapshot key bound).", fillName);
	} else if (effSnapKey != D2RL::Input::Key::None) {
		std::snprintf(keyHint, sizeof(keyHint), "%s = snapshot columns (no fill key bound; console: belt-fill).", snapName);
	} else {
		std::snprintf(keyHint, sizeof(keyHint), "no hotkeys bound - bind 'Refill Belt' / 'Snapshot Belt Columns' in the game's control settings (Options > Controls > D2RLoader category), or use console: belt-fill.");
	}
	std::snprintf(banner, sizeof(banner),
		"AutoBeltRefill v0.35.7 loaded. %s "
		"Console (Ctrl + `): belt-fill, belt-place, belt-policy, belt-config, belt-snapshot, belt-reset, belt-scan, belt-watch, belt-verify",
		keyHint);
	context->LogInfo(banner);
	if (g_config.autoRefill) {
		// The GameJoined listener restarts the watch chain whenever a game
		// begins; this load-time attempt covers a hot reload mid-game.
		const D2RL::Lifecycle::GameplayEventListener autoListener {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.flags      = 0,
			.kind       = D2RL::Lifecycle::GameplayEventKind::GameJoined,
			.callback   = OnAutoGameplayEvent,
			.userData   = nullptr,
		};
		const D2RL::LifecycleServiceV1* lifecycle = nullptr;
		if (context->QueryService(D2RL::ServiceId::Lifecycle, D2RL::LifecycleServiceV1Version, &lifecycle) == D2RL::ServiceQueryResult::Success
		    && D2RL::HasLifecycleServiceV1Field(lifecycle, D2RL::LifecycleServiceV1RequiredSize)) {
			D2RL::Lifecycle::ListenerHandle listenerHandle = D2RL::Lifecycle::InvalidHandle;
			if (lifecycle->registerGameplayEventListener(context, &autoListener, &listenerHandle) == D2RL::Lifecycle::Result::Success) {
				EnsureAutoWatch(context);
			} else {
				context->LogInfo("AutoBeltRefill: auto_refill: GameJoined listener registration failed - press F9 once in-game to start the watch chain.");
			}
		} else {
			context->LogInfo("AutoBeltRefill: auto_refill: lifecycle service unavailable - press F9 once in-game to start the watch chain.");
		}
	}
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_inventory       = nullptr;
	g_threads         = nullptr;
	g_input           = nullptr;
	g_hookLogContext  = nullptr;
	g_moduleBase      = 0;
	g_fillPending     = nullptr;
	g_fillRunning     = false;
	g_autoChainLive   = false;
	g_autoTickCount   = 0;
	g_autoPrevValid   = false;
	g_autoArmLeft     = 0;
	g_autoCooldownLeft = 0;
	g_policyPlayer    = D2RL::InvalidPlayerHandle;
	for (uint32_t column = 0; column < BeltColumns; ++column) {
		g_policy[column] = ColumnPolicy {};
	}
	g_lastBeltCount = 0;
	g_lastBeltValid = false;
}
