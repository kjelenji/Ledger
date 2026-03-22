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

## Why this is unique

- Zero friction: Documentation happens inside normal MR conversations.
- Intent over implementation: Captures human reasoning, not only code diffs.
- High-performance C++ core: Lightweight, type-safe logic pipe between webhook events and GitLab API.

## Project status

This repository currently contains a basic single-file template implementation in [lilbot.cpp](lilbot.cpp).

The template includes:
- Decision extraction using regex from note text
- Decision entry model (author, timestamp, reason)
- Summary Card Markdown renderer
- In-memory ledger store keyed by project and MR
- Webhook handler skeleton
- GitLab API adapter stub for upsert behavior
- Local demo payload in main for smoke testing

## Build and run

Requirements:
- g++ with C++17 support

Build:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic lilbot.cpp -o lilbot.exe
```

Run:

```powershell
.\lilbot.exe
```

Expected behavior:
- The app processes the sample payload.
- A rendered Summary Card is printed to stdout.

## Current file map

- [lilbot.cpp](lilbot.cpp): Bot template, extraction logic, summary rendering, and demo main.
- [README.md](README.md): Project overview and setup guide.

## Next implementation steps

1. Replace regex-based JSON extraction with a robust JSON library.
2. Add an HTTP server endpoint to receive GitLab webhooks.
3. Validate webhook authenticity using signed headers.
4. Implement real GitLab API calls:
- Find existing Summary Card comment
- Update if found
- Create if missing
5. Persist decision history in durable storage (database or file-backed store).
6. Add tests for parser, extractor, and summary rendering behavior.

## Example Summary Card

```markdown
<!-- architects-ledger-summary-card -->
## Architect's Ledger
Passive record of why key technical decisions were made in this MR.

| # | Timestamp (UTC) | Author | Decision |
|---|------------------|--------|----------|
| 1 | 2026-03-22T14:23:00Z | Ava Rivera | switching to C++ for lower latency |
```

## License

Add your preferred license file before publishing.
