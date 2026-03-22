#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ledgerbot {

struct DecisionEntry {
	std::string author;
	std::string timestamp;
	std::string reason;
};

struct MergeRequestContext {
	std::string projectId;
	std::string mrIid;
	std::string noteId;
	std::string author;
	std::string createdAt;
	std::string body;
};

class RegexExtractor {
public:
	static std::optional<std::string> extractDecisionReason(const std::string& noteBody) {
		// Supports: "#decision: reason", "#decision - reason", "#decision reason".
		static const std::regex kDecisionPattern(
			R"(#decision\s*(?::|-)?\s*([^\n\r]+))",
			std::regex::icase);

		std::smatch match;
		if (!std::regex_search(noteBody, match, kDecisionPattern)) {
			return std::nullopt;
		}

		if (match.size() < 2) {
			return std::nullopt;
		}

		std::string reason = trim(match[1].str());
		if (reason.empty()) {
			return std::nullopt;
		}
		return reason;
	}

private:
	static std::string trim(const std::string& input) {
		const char* whitespace = " \t\n\r\f\v";
		const std::size_t start = input.find_first_not_of(whitespace);
		if (start == std::string::npos) {
			return "";
		}
		const std::size_t end = input.find_last_not_of(whitespace);
		return input.substr(start, end - start + 1);
	}
};

class SummaryCard {
public:
	static std::string marker() {
		return "<!-- architects-ledger-summary-card -->";
	}

	static std::string render(const std::vector<DecisionEntry>& entries) {
		std::ostringstream out;
		out << marker() << "\n";
		out << "## Architect's Ledger\n";
		out << "Passive record of why key technical decisions were made in this MR.\n\n";

		if (entries.empty()) {
			out << "_No decisions captured yet. Tag comments with `#decision` to add one._\n";
			return out.str();
		}

		out << "| # | Timestamp (UTC) | Author | Decision |\n";
		out << "|---|------------------|--------|----------|\n";
		for (std::size_t i = 0; i < entries.size(); ++i) {
			out << "| " << (i + 1) << " | "
				<< sanitizeCell(entries[i].timestamp) << " | "
				<< sanitizeCell(entries[i].author) << " | "
				<< sanitizeCell(entries[i].reason) << " |\n";
		}
		return out.str();
	}

private:
	static std::string sanitizeCell(const std::string& value) {
		std::string clean;
		clean.reserve(value.size());
		for (char c : value) {
			if (c == '|') {
				clean += "\\|";
			} else if (c == '\n' || c == '\r') {
				clean += ' ';
			} else {
				clean += c;
			}
		}
		return clean;
	}
};

class GitLabAdapter {
public:
	explicit GitLabAdapter(std::string apiToken)
		: apiToken_(std::move(apiToken)) {}

	bool upsertSummaryCard(const MergeRequestContext& ctx, const std::string& cardBody) {
		// TODO: Replace with actual GitLab API calls.
		// 1) GET MR notes and find note containing SummaryCard::marker().
		// 2) If found, PUT update note body; else POST a new note.
		std::cout << "[GitLabAdapter] Upserting summary card for project=" << ctx.projectId
				  << " mr_iid=" << ctx.mrIid << "\n";
		std::cout << "---------- CARD START ----------\n";
		std::cout << cardBody << "\n";
		std::cout << "----------- CARD END -----------\n";
		(void)apiToken_;
		return true;
	}

private:
	std::string apiToken_;
};

class LedgerStore {
public:
	void append(const MergeRequestContext& ctx, const DecisionEntry& entry) {
		keyToEntries_[makeKey(ctx)].push_back(entry);
	}

	std::vector<DecisionEntry> list(const MergeRequestContext& ctx) const {
		const auto it = keyToEntries_.find(makeKey(ctx));
		if (it == keyToEntries_.end()) {
			return {};
		}
		return it->second;
	}

private:
	static std::string makeKey(const MergeRequestContext& ctx) {
		return ctx.projectId + ":" + ctx.mrIid;
	}

	std::unordered_map<std::string, std::vector<DecisionEntry>> keyToEntries_;
};

class ArchitectLedgerBot {
public:
	ArchitectLedgerBot(GitLabAdapter adapter, LedgerStore store)
		: adapter_(std::move(adapter)), store_(std::move(store)) {}

	bool handleWebhook(const std::string& rawJsonPayload,
					   const std::string& webhookSecretHeader,
					   const std::string& expectedSecret) {
		if (!verifyWebhookSecret(webhookSecretHeader, expectedSecret)) {
			std::cerr << "[Bot] Invalid webhook secret.\n";
			return false;
		}

		std::optional<MergeRequestContext> ctx = parseMergeRequestContext(rawJsonPayload);
		if (!ctx.has_value()) {
			std::cerr << "[Bot] Payload does not appear to be a supported MR note event.\n";
			return false;
		}

		std::optional<std::string> reason = RegexExtractor::extractDecisionReason(ctx->body);
		if (!reason.has_value()) {
			std::cout << "[Bot] No #decision tag found; ignoring note.\n";
			return true;
		}

		DecisionEntry entry{ctx->author, normalizeTimestamp(ctx->createdAt), reason.value()};
		store_.append(*ctx, entry);

		std::vector<DecisionEntry> decisions = store_.list(*ctx);
		const std::string card = SummaryCard::render(decisions);
		return adapter_.upsertSummaryCard(*ctx, card);
	}

private:
	// In production use constant-time comparison and signature validation.
	static bool verifyWebhookSecret(const std::string& received, const std::string& expected) {
		return !expected.empty() && received == expected;
	}

	static std::optional<MergeRequestContext> parseMergeRequestContext(const std::string& json) {
		// Minimal regex-based field extraction template to keep the example dependency-free.
		// Swap this with a robust JSON parser (e.g. nlohmann/json, simdjson, RapidJSON).
		const std::string projectId = extractJsonString(json, R"PAT("project"\s*:\s*\{[^\}]*"id"\s*:\s*([0-9]+))PAT", 1);
		const std::string mrIid = extractJsonString(json, R"PAT("merge_request"\s*:\s*\{[^\}]*"iid"\s*:\s*([0-9]+))PAT", 1);
		const std::string noteId = extractJsonString(json, R"PAT("object_attributes"\s*:\s*\{[^\}]*"id"\s*:\s*([0-9]+))PAT", 1);
		const std::string body = extractJsonString(json, R"PAT("object_attributes"\s*:\s*\{[^\}]*"note"\s*:\s*"([^"]*)")PAT", 1);
		const std::string author = extractJsonString(json, R"PAT("user"\s*:\s*\{[^\}]*"name"\s*:\s*"([^"]+)")PAT", 1);
		const std::string createdAt = extractJsonString(json, R"PAT("object_attributes"\s*:\s*\{[^\}]*"created_at"\s*:\s*"([^"]+)")PAT", 1);

		if (projectId.empty() || mrIid.empty() || body.empty()) {
			return std::nullopt;
		}

		MergeRequestContext ctx;
		ctx.projectId = projectId;
		ctx.mrIid = mrIid;
		ctx.noteId = noteId;
		ctx.body = unescapeJsonString(body);
		ctx.author = author.empty() ? "unknown" : author;
		ctx.createdAt = createdAt.empty() ? currentUtcIso8601() : createdAt;
		return ctx;
	}

	static std::string extractJsonString(const std::string& json,
										 const std::string& pattern,
										 int groupIndex) {
		std::regex r(pattern, std::regex::icase);
		std::smatch match;
		if (!std::regex_search(json, match, r)) {
			return "";
		}
		if (groupIndex < 0 || static_cast<std::size_t>(groupIndex) >= match.size()) {
			return "";
		}
		return match[static_cast<std::size_t>(groupIndex)].str();
	}

	static std::string unescapeJsonString(const std::string& input) {
		std::string out;
		out.reserve(input.size());

		for (std::size_t i = 0; i < input.size(); ++i) {
			if (input[i] == '\\' && i + 1 < input.size()) {
				const char next = input[i + 1];
				switch (next) {
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case '\\': out += '\\'; break;
				case '"': out += '"'; break;
				default: out += next; break;
				}
				++i;
				continue;
			}
			out += input[i];
		}
		return out;
	}

	static std::string currentUtcIso8601() {
		const auto now = std::chrono::system_clock::now();
		const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);

		std::tm utc {};
#ifdef _WIN32
		gmtime_s(&utc, &nowTime);
#else
		gmtime_r(&nowTime, &utc);
#endif

		std::ostringstream out;
		out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
		return out.str();
	}

	static std::string normalizeTimestamp(const std::string& timestamp) {
		return timestamp.empty() ? currentUtcIso8601() : timestamp;
	}

	GitLabAdapter adapter_;
	LedgerStore store_;
};

} // namespace ledgerbot

int main() {
	using namespace ledgerbot;

	const std::string fakePayload =
		R"({
			"project": { "id": 42 },
			"merge_request": { "iid": 128 },
			"user": { "name": "Ava Rivera" },
			"object_attributes": {
				"id": 911,
				"created_at": "2026-03-22T14:23:00Z",
				"note": "#decision: switching to C++ for lower latency"
			}
		})";

	GitLabAdapter adapter("GITLAB_API_TOKEN_PLACEHOLDER");
	LedgerStore store;
	ArchitectLedgerBot bot(std::move(adapter), std::move(store));

	const bool ok = bot.handleWebhook(
		fakePayload,
		"MY_WEBHOOK_SECRET",
		"MY_WEBHOOK_SECRET");

	std::cout << (ok ? "[Main] Processed webhook successfully.\n"
					 : "[Main] Failed to process webhook.\n");
	return ok ? 0 : 1;
}
