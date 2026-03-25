#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define LEDGERBOT_HAS_LIBCURL 1
#else
#define LEDGERBOT_HAS_LIBCURL 0
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

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

struct ContextualSummary {
std::string headline;
std::string summary;
std::string considerations;

bool hasContent() const {
return !headline.empty() || !summary.empty() || !considerations.empty();
}
};

class RegexExtractor {
public:
static std::optional<std::string> extractDecisionReason(const std::string& noteBody) {
static const std::regex kDecisionPattern(R"(#decision\s*(?::|-)?\s*([^\n\r]+))", std::regex::icase);
std::smatch match;
if (!std::regex_search(noteBody, match, kDecisionPattern) || match.size() < 2) {
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
return "<!-- ledger--bot--summary-card -->";
}

static std::string render(const std::vector<DecisionEntry>& entries,
  const std::optional<ContextualSummary>& contextualSummary = std::nullopt) {
std::ostringstream out;
out << marker() << "\n";
out << "## Ledger Bot\n";
out << "Passive record of why key technical decisions were made in this MR.\n\n";

if (contextualSummary.has_value() && contextualSummary->hasContent()) {
if (!contextualSummary->headline.empty()) {
out << "**Context:** " << sanitizeCell(contextualSummary->headline) << "\n\n";
}
if (!contextualSummary->summary.empty()) {
out << sanitizeParagraph(contextualSummary->summary) << "\n\n";
}
if (!contextualSummary->considerations.empty()) {
out << "**Considerations:** " << sanitizeParagraph(contextualSummary->considerations) << "\n\n";
}
}

if (entries.empty()) {
out << "_No decisions captured yet. Tag comments with `#decision` to add one._\n";
return out.str();
}

out << "### Decisions\n\n";
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

static std::string sanitizeParagraph(const std::string& value) {
std::string clean = sanitizeCell(value);
for (char& c : clean) {
if (c == '\n' || c == '\r') {
c = ' ';
}
}
return clean;
}
};

class AnthropicSummaryGenerator {
public:
AnthropicSummaryGenerator(std::string apiKey, std::string model = "claude-3-5-sonnet-latest")
: apiKey_(std::move(apiKey)), model_(std::move(model)) {}

std::optional<ContextualSummary> generate(const MergeRequestContext& ctx,
 const std::vector<DecisionEntry>& entries) const {
if (apiKey_.empty() || entries.empty()) {
return std::nullopt;
}

const std::string requestPath = "anthropic_request.json";
const std::string responsePath = "anthropic_response.json";
if (!writeRequestFile(requestPath, buildRequestPayload(ctx, entries))) {
std::cerr << "[Anthropic] Failed to write request payload.\n";
return std::nullopt;
}

const bool ok = runCurlRequest(requestPath, responsePath);
std::remove(requestPath.c_str());
if (!ok) {
std::remove(responsePath.c_str());
return std::nullopt;
}

const std::optional<std::string> responseBody = readFile(responsePath);
std::remove(responsePath.c_str());
if (!responseBody.has_value()) {
std::cerr << "[Anthropic] Failed to read response body.\n";
return std::nullopt;
}
return parseResponse(*responseBody);
}

private:
std::string apiKey_;
std::string model_;

static std::string escapeJsonString(const std::string& input) {
std::string escaped;
escaped.reserve(input.size());
for (char c : input) {
switch (c) {
case '\\': escaped += "\\\\"; break;
case '"': escaped += "\\\""; break;
case '\n': escaped += "\\n"; break;
case '\r': escaped += "\\r"; break;
case '\t': escaped += "\\t"; break;
default: escaped += c; break;
}
}
return escaped;
}

static std::optional<std::string> readFile(const std::string& path) {
std::ifstream input(path);
if (!input.is_open()) {
return std::nullopt;
}
std::ostringstream buffer;
buffer << input.rdbuf();
return buffer.str();
}

static bool writeRequestFile(const std::string& path, const std::string& payload) {
std::ofstream output(path, std::ios::trunc);
if (!output.is_open()) {
return false;
}
output << payload;
return output.good();
}

std::string buildPrompt(const MergeRequestContext& ctx,
const std::vector<DecisionEntry>& entries) const {
std::ostringstream prompt;
prompt << "You are generating a concise merge request summary card. "
   << "Given the decision log below, return valid JSON with keys "
   << "headline, summary, considerations. "
   << "Keep each field plain text. summary and considerations should each be at most 2 sentences.\n\n";
prompt << "Project ID: " << ctx.projectId << "\n";
prompt << "Merge Request IID: " << ctx.mrIid << "\n";
prompt << "Latest note author: " << ctx.author << "\n\n";
prompt << "Decision entries:\n";
for (std::size_t i = 0; i < entries.size(); ++i) {
prompt << i + 1 << ". [" << entries[i].timestamp << "] "
   << entries[i].author << ": " << entries[i].reason << "\n";
}
prompt << "\nReturn JSON only.";
return prompt.str();
}

std::string buildRequestPayload(const MergeRequestContext& ctx,
 const std::vector<DecisionEntry>& entries) const {
std::ostringstream payload;
payload << "{";
payload << "\"model\":\"" << escapeJsonString(model_) << "\",";
payload << "\"max_tokens\":400,";
payload << "\"temperature\":0.2,";
payload << "\"messages\":[{";
payload << "\"role\":\"user\",";
payload << "\"content\":\"" << escapeJsonString(buildPrompt(ctx, entries)) << "\"";
payload << "}]}";
return payload.str();
}

bool runCurlRequest(const std::string& requestPath, const std::string& responsePath) const {
std::ostringstream command;
command << "curl -sS https://api.anthropic.com/v1/messages ";
command << "-H \"x-api-key: " << apiKey_ << "\" ";
command << "-H \"anthropic-version: 2023-06-01\" ";
command << "-H \"content-type: application/json\" ";
command << "--data @\"" << requestPath << "\" ";
command << "> \"" << responsePath << "\"";

const int exitCode = std::system(command.str().c_str());
if (exitCode != 0) {
std::cerr << "[Anthropic] curl request failed with exit code " << exitCode << ".\n";
return false;
}
return true;
}

static std::optional<std::string> extractJsonField(const std::string& json, const std::string& key) {
const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
std::smatch match;
if (!std::regex_search(json, match, pattern) || match.size() < 2) {
return std::nullopt;
}
return unescapeJsonString(match[1].str());
}

static std::optional<std::string> extractContentText(const std::string& response) {
const std::regex pattern(R"PAT("content"\s*:\s*\[\s*\{[^\}]*"text"\s*:\s*"((?:\\.|[^"\\])*)")PAT");
std::smatch match;
if (!std::regex_search(response, match, pattern) || match.size() < 2) {
return std::nullopt;
}
return unescapeJsonString(match[1].str());
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

std::optional<ContextualSummary> parseResponse(const std::string& response) const {
const std::optional<std::string> contentText = extractContentText(response);
if (!contentText.has_value()) {
std::cerr << "[Anthropic] Could not extract text content from response.\n";
return std::nullopt;
}

ContextualSummary summary;
summary.headline = extractJsonField(*contentText, "headline").value_or("");
summary.summary = extractJsonField(*contentText, "summary").value_or("");
summary.considerations = extractJsonField(*contentText, "considerations").value_or("");
if (!summary.hasContent()) {
std::cerr << "[Anthropic] Response did not contain contextual summary fields.\n";
return std::nullopt;
}
return summary;
}
};

class GitLabAdapter {
public:
GitLabAdapter(std::string apiToken, std::string baseApiUrl)
: apiToken_(std::move(apiToken)), baseApiUrl_(std::move(baseApiUrl)) {}

bool upsertSummaryCard(const MergeRequestContext& ctx, const std::string& cardBody) {
if (apiToken_.empty()) {
std::cerr << "[GitLabAdapter] Missing GITLAB_API_TOKEN; cannot upsert notes.\n";
return false;
}

const std::string notesUrl = baseApiUrl_ + "/projects/" + ctx.projectId + "/merge_requests/" + ctx.mrIid + "/notes";
long statusCode = 0;
const std::optional<std::string> notesResponse = performRequest("GET", notesUrl, "", statusCode);
if (!notesResponse.has_value() || statusCode < 200 || statusCode >= 300) {
std::cerr << "[GitLabAdapter] Failed to fetch notes. HTTP status=" << statusCode << "\n";
return false;
}

const std::optional<std::string> existingNoteId = findSummaryNoteId(*notesResponse, SummaryCard::marker());
const std::string encodedBody = "body=" + urlEncode(cardBody);

if (existingNoteId.has_value()) {
const std::string updateUrl = notesUrl + "/" + *existingNoteId;
const std::optional<std::string> updateResponse = performRequest("PUT", updateUrl, encodedBody, statusCode);
if (!updateResponse.has_value() || statusCode < 200 || statusCode >= 300) {
std::cerr << "[GitLabAdapter] Failed to update existing summary note. HTTP status=" << statusCode << "\n";
return false;
}
std::cout << "[GitLabAdapter] Updated summary note id=" << *existingNoteId << "\n";
return true;
}

const std::optional<std::string> createResponse = performRequest("POST", notesUrl, encodedBody, statusCode);
if (!createResponse.has_value() || statusCode < 200 || statusCode >= 300) {
std::cerr << "[GitLabAdapter] Failed to create summary note. HTTP status=" << statusCode << "\n";
return false;
}
std::cout << "[GitLabAdapter] Created new summary note.\n";
return true;
}

private:
std::string apiToken_;
std::string baseApiUrl_;

#if LEDGERBOT_HAS_LIBCURL
struct CurlBuffer {
std::string data;
};

static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
const size_t totalSize = size * nmemb;
CurlBuffer* buffer = static_cast<CurlBuffer*>(userp);
buffer->data.append(static_cast<char*>(contents), totalSize);
return totalSize;
}

static std::string urlEncode(const std::string& value) {
CURL* curl = curl_easy_init();
if (curl == nullptr) {
return "";
}
char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
std::string out = encoded != nullptr ? encoded : "";
if (encoded != nullptr) {
curl_free(encoded);
}
curl_easy_cleanup(curl);
return out;
}

#else
static std::string urlEncode(const std::string& value) {
   std::ostringstream out;
   const char hex[] = "0123456789ABCDEF";
   for (unsigned char c : value) {
      const bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
      if (unreserved) {
         out << static_cast<char>(c);
      } else {
         out << '%';
         out << hex[(c >> 4) & 0xF];
         out << hex[c & 0xF];
      }
   }
   return out.str();
}
#endif

std::optional<std::string> performRequest(const std::string& method,
 const std::string& url,
 const std::string& formEncodedBody,
 long& statusCode) {
#if LEDGERBOT_HAS_LIBCURL
statusCode = 0;
CURL* curl = curl_easy_init();
if (curl == nullptr) {
std::cerr << "[GitLabAdapter] curl_easy_init failed.\n";
return std::nullopt;
}

CurlBuffer response;
struct curl_slist* headers = nullptr;
headers = curl_slist_append(headers, ("PRIVATE-TOKEN: " + apiToken_).c_str());
headers = curl_slist_append(headers, "Accept: application/json");

curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCallback);
curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

if (method == "PUT") {
headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
curl_easy_setopt(curl, CURLOPT_POSTFIELDS, formEncodedBody.c_str());
} else if (method == "POST") {
headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
curl_easy_setopt(curl, CURLOPT_POST, 1L);
curl_easy_setopt(curl, CURLOPT_POSTFIELDS, formEncodedBody.c_str());
} else {
curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
}

const CURLcode result = curl_easy_perform(curl);
if (result != CURLE_OK) {
std::cerr << "[GitLabAdapter] curl_easy_perform failed: " << curl_easy_strerror(result) << "\n";
curl_slist_free_all(headers);
curl_easy_cleanup(curl);
return std::nullopt;
}
curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);

curl_slist_free_all(headers);
curl_easy_cleanup(curl);
return response.data;
#else
   statusCode = 0;
   const std::string responsePath = "gitlab_http_response.json";
   std::ostringstream command;
   command << "curl -sS -X " << method << " \"" << url << "\" ";
   command << "-H \"PRIVATE-TOKEN: " << apiToken_ << "\" ";
   command << "-H \"Accept: application/json\" ";
   if (method == "POST" || method == "PUT") {
      command << "-H \"Content-Type: application/x-www-form-urlencoded\" ";
      command << "--data \"" << formEncodedBody << "\" ";
   }
   command << "-o \"" << responsePath << "\"";

   const int exitCode = std::system(command.str().c_str());
   if (exitCode != 0) {
      std::cerr << "[GitLabAdapter] curl CLI request failed with exit code " << exitCode << "\n";
      std::remove(responsePath.c_str());
      statusCode = 500;
      return std::nullopt;
   }

   std::ifstream input(responsePath);
   if (!input.is_open()) {
      std::cerr << "[GitLabAdapter] Failed to open response file.\n";
      std::remove(responsePath.c_str());
      statusCode = 500;
      return std::nullopt;
   }
   std::ostringstream body;
   body << input.rdbuf();
   std::remove(responsePath.c_str());
   statusCode = 200;
   return body.str();
#endif
}

static std::string jsonUnescape(const std::string& input) {
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

static std::optional<std::string> findSummaryNoteId(const std::string& notesJson, const std::string& marker) {
// Regex parser for note objects; replace with a JSON parser in production.
const std::regex notePattern(R"PAT(\{[\s\S]*?"id"\s*:\s*([0-9]+)[\s\S]*?"body"\s*:\s*"((?:\\.|[^"\\])*)")PAT");
auto begin = std::sregex_iterator(notesJson.begin(), notesJson.end(), notePattern);
auto end = std::sregex_iterator();
for (auto it = begin; it != end; ++it) {
if (it->size() < 3) {
continue;
}
const std::string body = jsonUnescape((*it)[2].str());
if (body.find(marker) != std::string::npos) {
return (*it)[1].str();
}
}
return std::nullopt;
}
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

class DevLedgerBot {
public:
DevLedgerBot(GitLabAdapter adapter, LedgerStore store, AnthropicSummaryGenerator summaryGenerator)
: adapter_(std::move(adapter)), store_(std::move(store)), summaryGenerator_(std::move(summaryGenerator)) {}

bool handleWebhook(const std::string& rawJsonPayload,
   const std::string& webhookSecretHeader,
   const std::string& expectedSecret) {
if (!verifyWebhookSecret(webhookSecretHeader, expectedSecret)) {
std::cerr << "[Bot] Invalid webhook secret.\n";
return false;
}

std::optional<MergeRequestContext> ctx = parseMergeRequestContext(rawJsonPayload);
if (!ctx.has_value()) {
std::cerr << "[Bot] Payload is not a supported MR note event.\n";
return false;
}

std::optional<std::string> reason = RegexExtractor::extractDecisionReason(ctx->body);
if (!reason.has_value()) {
std::cout << "[Bot] No #decision tag found; ignoring note.\n";
return true;
}

DecisionEntry entry{ctx->author, normalizeTimestamp(ctx->createdAt), reason.value()};
store_.append(*ctx, entry);

const std::vector<DecisionEntry> decisions = store_.list(*ctx);
const std::optional<ContextualSummary> contextualSummary = summaryGenerator_.generate(*ctx, decisions);
const std::string card = SummaryCard::render(decisions, contextualSummary);
return adapter_.upsertSummaryCard(*ctx, card);
}

private:
static bool verifyWebhookSecret(const std::string& received, const std::string& expected) {
return !expected.empty() && received == expected;
}

static std::optional<MergeRequestContext> parseMergeRequestContext(const std::string& json) {
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
std::tm utc{};
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
AnthropicSummaryGenerator summaryGenerator_;
};

#ifdef _WIN32
class WebhookServer {
public:
WebhookServer(DevLedgerBot& bot, std::string expectedSecret, int port)
: bot_(bot), expectedSecret_(std::move(expectedSecret)), port_(port) {}

bool run() {
WSADATA wsaData;
if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
std::cerr << "[Server] WSAStartup failed.\n";
return false;
}

SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
if (serverSocket == INVALID_SOCKET) {
std::cerr << "[Server] Failed to create socket.\n";
WSACleanup();
return false;
}

sockaddr_in service{};
service.sin_family = AF_INET;
service.sin_addr.s_addr = INADDR_ANY;
service.sin_port = htons(static_cast<u_short>(port_));

if (bind(serverSocket, reinterpret_cast<SOCKADDR*>(&service), sizeof(service)) == SOCKET_ERROR) {
std::cerr << "[Server] Bind failed on port " << port_ << ".\n";
closesocket(serverSocket);
WSACleanup();
return false;
}

if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
std::cerr << "[Server] Listen failed.\n";
closesocket(serverSocket);
WSACleanup();
return false;
}

std::cout << "[Server] Listening on port " << port_ << " for POST /webhook\n";
while (true) {
SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
if (clientSocket == INVALID_SOCKET) {
std::cerr << "[Server] Accept failed.\n";
continue;
}
handleConnection(clientSocket);
closesocket(clientSocket);
}
}

private:
DevLedgerBot& bot_;
std::string expectedSecret_;
int port_;

static std::string toLower(std::string value) {
std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
return static_cast<char>(std::tolower(c));
});
return value;
}

static std::string trim(const std::string& input) {
const char* whitespace = " \t\r\n";
const std::size_t start = input.find_first_not_of(whitespace);
if (start == std::string::npos) {
return "";
}
const std::size_t end = input.find_last_not_of(whitespace);
return input.substr(start, end - start + 1);
}

static bool sendResponse(SOCKET clientSocket, int statusCode, const std::string& body) {
std::ostringstream response;
response << "HTTP/1.1 " << statusCode << " " << statusText(statusCode) << "\r\n";
response << "Content-Type: text/plain\r\n";
response << "Content-Length: " << body.size() << "\r\n";
response << "Connection: close\r\n\r\n";
response << body;
const std::string data = response.str();
return send(clientSocket, data.c_str(), static_cast<int>(data.size()), 0) != SOCKET_ERROR;
}

static const char* statusText(int statusCode) {
switch (statusCode) {
case 200: return "OK";
case 400: return "Bad Request";
case 401: return "Unauthorized";
case 404: return "Not Found";
case 500: return "Internal Server Error";
default: return "Unknown";
}
}

void handleConnection(SOCKET clientSocket) {
std::string request;
char buffer[4096];
while (true) {
const int received = recv(clientSocket, buffer, sizeof(buffer), 0);
if (received <= 0) {
break;
}
request.append(buffer, received);
if (request.find("\r\n\r\n") != std::string::npos) {
break;
}
}

if (request.empty()) {
sendResponse(clientSocket, 400, "Empty request");
return;
}

const std::size_t headersEnd = request.find("\r\n\r\n");
if (headersEnd == std::string::npos) {
sendResponse(clientSocket, 400, "Malformed request headers");
return;
}

const std::string headersPart = request.substr(0, headersEnd);
std::string bodyPart = request.substr(headersEnd + 4);

std::istringstream headersStream(headersPart);
std::string requestLine;
std::getline(headersStream, requestLine);
if (!requestLine.empty() && requestLine.back() == '\r') {
requestLine.pop_back();
}

std::istringstream reqLineStream(requestLine);
std::string method;
std::string path;
reqLineStream >> method >> path;

std::unordered_map<std::string, std::string> headers;
std::string line;
while (std::getline(headersStream, line)) {
if (!line.empty() && line.back() == '\r') {
line.pop_back();
}
const std::size_t colon = line.find(':');
if (colon == std::string::npos) {
continue;
}
const std::string key = toLower(trim(line.substr(0, colon)));
const std::string value = trim(line.substr(colon + 1));
headers[key] = value;
}

std::size_t contentLength = 0;
auto contentLengthIt = headers.find("content-length");
if (contentLengthIt != headers.end()) {
contentLength = static_cast<std::size_t>(std::strtoul(contentLengthIt->second.c_str(), nullptr, 10));
}

while (bodyPart.size() < contentLength) {
const int received = recv(clientSocket, buffer, sizeof(buffer), 0);
if (received <= 0) {
break;
}
bodyPart.append(buffer, received);
}

if (method != "POST" || path != "/webhook") {
sendResponse(clientSocket, 404, "Only POST /webhook is supported");
return;
}

const std::string webhookToken = headers.count("x-gitlab-token") > 0 ? headers["x-gitlab-token"] : "";
if (webhookToken.empty()) {
sendResponse(clientSocket, 401, "Missing X-Gitlab-Token");
return;
}

const bool ok = bot_.handleWebhook(bodyPart.substr(0, contentLength), webhookToken, expectedSecret_);
sendResponse(clientSocket, ok ? 200 : 400, ok ? "Processed" : "Rejected");
}
};
#endif

} // namespace ledgerbot

int main(int argc, char** argv) {
using namespace ledgerbot;

#if LEDGERBOT_HAS_LIBCURL
curl_global_init(CURL_GLOBAL_DEFAULT);
#endif

const char* gitlabTokenEnv = std::getenv("GITLAB_API_TOKEN");
const char* gitlabBaseUrlEnv = std::getenv("GITLAB_BASE_URL");
const char* webhookSecretEnv = std::getenv("WEBHOOK_SECRET");
const char* anthropicApiKeyEnv = std::getenv("ANTHROPIC_API_KEY");
const char* portEnv = std::getenv("PORT");

const std::string gitlabToken = gitlabTokenEnv != nullptr ? gitlabTokenEnv : "";
const std::string gitlabBaseUrl = gitlabBaseUrlEnv != nullptr ? gitlabBaseUrlEnv : "https://gitlab.com/api/v4";
const std::string webhookSecret = webhookSecretEnv != nullptr ? webhookSecretEnv : "";
const std::string anthropicApiKey = anthropicApiKeyEnv != nullptr ? anthropicApiKeyEnv : "";
const int port = portEnv != nullptr ? std::atoi(portEnv) : 8080;

GitLabAdapter adapter(gitlabToken, gitlabBaseUrl);
LedgerStore store;
AnthropicSummaryGenerator summaryGenerator(anthropicApiKey);
DevLedgerBot bot(std::move(adapter), std::move(store), std::move(summaryGenerator));

if (argc > 1 && std::string(argv[1]) == "--demo") {
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

const bool ok = bot.handleWebhook(fakePayload, webhookSecret, webhookSecret);
std::cout << (ok ? "[Main] Demo processed successfully.\n" : "[Main] Demo failed.\n");
#if LEDGERBOT_HAS_LIBCURL
curl_global_cleanup();
#endif
return ok ? 0 : 1;
}

#ifdef _WIN32
if (webhookSecret.empty()) {
std::cerr << "[Main] WEBHOOK_SECRET is required in server mode.\n";
#if LEDGERBOT_HAS_LIBCURL
curl_global_cleanup();
#endif
return 1;
}
std::cout << "[Main] Starting webhook server.\n";
std::cout << "[Main] GITLAB_BASE_URL=" << gitlabBaseUrl << "\n";
WebhookServer server(bot, webhookSecret, port);
const bool serverOk = server.run();
#if LEDGERBOT_HAS_LIBCURL
curl_global_cleanup();
#endif
return serverOk ? 0 : 1;
#else
std::cerr << "[Main] Server mode currently implemented for Windows build in this template.\n";
#if LEDGERBOT_HAS_LIBCURL
curl_global_cleanup();
#endif
return 1;
#endif
}
