# The Dev's Ledger

The Dev's Ledger is a passive documentation bot for GitLab Merge Requests.
It captures the why behind code changes directly from MR discussion comments and maintains a living summary card at the top of the thread.

## What problem this solves

Most teams lose architectural intent after merge:
- Code shows what changed.
- Commit history shows when it changed.
- MR comments contain why it changed, but that context is scattered and easy to lose.

This bot keeps the reasoning visible and durable without adding documentation overhead.

## Core idea

Developers keep working normally in MR comments.
When someone writes a decision note with the tag below, the bot captures it automatically:

- #decision: switching to C++ for lower latency

## 3-step flow

1. Trigger
A developer posts a GitLab MR comment containing #decision.

2. Capture
The C++ middleware receives a GitLab webhook and extracts:
- Decision reason text
- Author
- Timestamp
- MR context (project and MR identifiers)

3. Ledger
The bot creates or updates one Summary Card comment at the top of the MR.
That card becomes a living table of key technical decisions for the feature.

## How card summaries are made

1. Webhook received
The bot receives a GitLab note webhook payload and verifies the shared secret.

2. MR note fields extracted
From the webhook JSON, it pulls project ID, MR IID, note text, author, and timestamp.

3. Decision tag detected
The note text is checked for a `#decision` tag (for example: `#decision: move cache to Redis`).
If no tag is present, the note is ignored.

4. Decision entry created
When a tag is found, the bot creates one decision entry with:
- Author
- Timestamp (or current UTC if missing)
- Decision reason text

5. Entry stored by MR
The decision is appended to an in-memory list keyed by `projectId:mrIid`, so each MR has its own running history.

6. Markdown card rendered
The bot rebuilds the full Summary Card Markdown table from all entries for that MR.
It includes a hidden marker comment so the same card can be found later.

If `ANTHROPIC_API_KEY` is configured, the bot also asks Anthropic to generate a short contextual headline, summary, and considerations block, which is inserted above the decision table.

7. Card upserted in GitLab
The adapter either updates the existing marker-based card note or creates a new one if none exists.

## Why this is unique

- Zero friction: Documentation happens inside normal MR conversations.
- Intent over implementation: Captures human reasoning, not only code diffs.
- High-performance C++ core: Lightweight, type-safe logic pipe between webhook events and GitLab API.

## Project status

This repository currently contains a basic single-file template implementation in [lilbot.cpp](lilbot.cpp).

The template includes:
- Decision extraction using regex from note text
- Optional Anthropic-generated contextual summary section
- Decision entry model (author, timestamp, reason)
- Summary Card Markdown renderer
- In-memory ledger store keyed by project and MR
- Windows HTTP server with `POST /webhook`
- `X-Gitlab-Token` secret verification
- GitLab notes upsert flow (GET notes, update existing marker note, or create new note)
- Local demo mode (`--demo`)

## Build and run

Requirements:
- g++ with C++17 support
- `curl` CLI available on PATH

Optional environment variables:
- `ANTHROPIC_API_KEY`: Enables contextual summary generation

Required environment variables for server mode:
- `WEBHOOK_SECRET`: Shared secret that must match GitLab webhook token
- `GITLAB_API_TOKEN`: GitLab Personal/Project Access Token with `api` scope

Optional environment variables for server mode:
- `GITLAB_BASE_URL`: Defaults to `https://gitlab.com/api/v4`
- `PORT`: Defaults to `8080`

Build:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic lilbot.cpp -o lilbot.exe -lws2_32
```

Run:

```powershell
\# server mode
$env:WEBHOOK_SECRET="your_webhook_secret"
$env:GITLAB_API_TOKEN="your_gitlab_api_token"
.\lilbot.exe
```

Run with Anthropic enabled:

```powershell
$env:WEBHOOK_SECRET="your_webhook_secret"
$env:GITLAB_API_TOKEN="your_gitlab_api_token"
$env:ANTHROPIC_API_KEY="your_anthropic_api_key"
.\lilbot.exe
```

Run local demo mode (no HTTP server):

```powershell
$env:WEBHOOK_SECRET="demo-secret"
.\lilbot.exe --demo
```

Expected behavior:
- In server mode, the app listens on `POST /webhook`.
- It validates `X-Gitlab-Token` against `WEBHOOK_SECRET`.
- It fetches MR notes, then updates or creates the summary card note in GitLab.

## Current file map

- [lilbot.cpp](lilbot.cpp): Bot template, extraction logic, summary rendering, and demo main.
- [README.md](README.md): Project overview and setup guide.

## Next implementation steps

1. Replace regex-based JSON extraction with a robust JSON library.
2. Add HTTPS termination and reverse-proxy deployment guidance.
3. Persist decision history in durable storage (SQLite/PostgreSQL).
4. Add retries and stronger HTTP status handling for external API calls.
5. Add tests for parser, webhook handling, and GitLab adapter behavior.

## Example Summary Card

```markdown
<!-- ledger--bot--summary-card -->
## Ledger Bot
Passive record of why key technical decisions were made in this MR.

**Context:** Performance-driven delivery choices

The decision history shows a bias toward lower-latency implementation choices for this merge request.

**Considerations:** Validate the operational tradeoffs and maintenance cost of the lower-level implementation as the feature evolves.

### Decisions

| # | Timestamp (UTC) | Author | Decision |
|---|------------------|--------|----------|
| 1 | 2026-03-22T14:23:00Z | Ava Rivera | switching to C++ for lower latency |
```

## License

Add your preferred license file before publishing.
