---
name: feishu-doc-ops
description: Read Feishu shared documents, templates, and markdown files through OAuth and OpenAPI instead of brittle browser scraping. Use when Codex needs to inspect a Feishu doc or file, download a shared markdown template such as PR_TEMPLATE.md, or turn Feishu content into local text that can drive follow-up edits such as PR descriptions.
---

# Feishu Doc Ops

## Overview

Use this skill when the target content lives in Feishu and the browser path is blocked, unreadable, or unstable.
Prefer Feishu OpenAPI over browser DOM extraction whenever the user needs the actual document text.

## Workflow

1. Decide whether the target is a docx document or a plain file.
   A Feishu share link such as `https://my.feishu.cn/file/<token>` is not enough to assume `docx`.

2. If the content must be read and no direct API token is known, authorize the user once.
   Use a temporary local callback such as `http://127.0.0.1:8765/feishu/callback`.
   Request these scopes when needed:
   - `drive:drive` for search and file download
   - `docx:document:readonly` for docx title and raw content
   - `auth:user.id:read` only when user identity helps with debugging or auxiliary lookup

3. Exchange the returned `code` through:
   `POST https://open.feishu.cn/open-apis/authen/v2/oauth/token`
   Use the resulting `user_access_token` for all user-visible document reads.

4. Locate the real token and type with:
   `POST https://open.feishu.cn/open-apis/suite/docs-api/search/object`
   Search by document title when the share token is ambiguous.
   This is the safest way to distinguish `file` from `docx`.

5. Read by type:
   - `file`: `GET https://open.feishu.cn/open-apis/drive/v1/files/{file_token}/download`
   - `docx`: `GET https://open.feishu.cn/open-apis/docx/v1/documents/{document_id}` and `GET https://open.feishu.cn/open-apis/docx/v1/documents/{document_id}/raw_content`

6. Convert the downloaded or raw content into the concrete artifact the user asked for.
   Example: read `PR_TEMPLATE.md`, then rewrite GitHub PR titles and bodies from that template.

## Guardrails

- Do not assume a `/file/<token>` share URL is a docx document id.
- Do not depend on Chrome extension DOM access for `my.feishu.cn`; OpenAPI is the primary path.
- Treat a blocked or blank callback page as non-fatal if the local callback file contains a fresh `code` and expected `state`.
- When a document search returns `docs_type: file`, use the file download API even if the title ends with `.md`.

## Known Good Case

The share link `https://my.feishu.cn/file/EC2qbQy3yoefO0x5U8VcEGLfnvd` resolves through search as:
- `docs_token`: `EC2qbQy3yoefO0x5U8VcEGLfnvd`
- `docs_type`: `file`
- `title`: `PR_TEMPLATE.md`

That object must be downloaded with `drive/v1/files/{file_token}/download`, not read through docx APIs.
