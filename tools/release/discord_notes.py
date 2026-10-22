# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Posts GitHub release notes to a Discord channel webhook, as embeds.

Only the change list goes to Discord: the "What's new", "Fixed in" and "What's in this
release" sections with their bullets, under "vX is out!". The setup steps, notes and build
info stay on the GitHub release, which the title links to.

Used by .github/workflows/discord-release-notes.yml. The webhook URL comes from the
DISCORD_WEBHOOK environment variable and is never printed.

  discord_notes.py release <event.json>     post the release of a `release` event
  discord_notes.py tag <tag>                post one release by its tag
  discord_notes.py backfill [--no-pre]      post every published release, oldest first

GITHUB_REPOSITORY and GITHUB_TOKEN name the repository and read its releases.
Only the standard library, so the workflow needs no pip install.
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

# Discord's limits: embed title 256, description 4096, all embeds of one message 6000.
TITLE_LIMIT = 256
DESCRIPTION_LIMIT = 4096
MESSAGE_LIMIT = 6000
COLOR = 0xD9B878  # the launcher's gold


def to_discord(markdown: str) -> str:
    """GitHub release markdown to what a Discord embed shows well."""
    text = (markdown or "").replace("\r\n", "\n")
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    text = re.sub(r"^#{1,6}\s+(.+?)\s*#*\s*$", r"**\1**", text, flags=re.M)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


# A section heading: "**What's new since v1.1.3**" or "### Fixed in v0.1.1".
SECTION = re.compile(r"^(?:#{1,6}\s+|\*\*)(what's new|what’s new|fixed in|what's in this release)(.*?)(?:\*\*)?\s*$", re.I)


def changes(markdown: str) -> str:
    """Just the release's change list: each change section's heading and its bullets."""
    lines = (markdown or "").replace("\r\n", "\n").split("\n")
    out, inside = [], False
    for line in lines:
        match = SECTION.match(line.strip())
        if match:
            title = match.group(1)[0].upper() + match.group(1)[1:]
            rest = match.group(2).strip()
            if rest and not rest.lower().startswith("since"):
                title += " " + rest
            if out:
                out.append("")
            out.append(f"**{title}**")
            inside = True
        elif inside and (line.lstrip().startswith(("- ", "* ")) or (line.startswith("  ") and line.strip())):
            out.append(line.rstrip())
        elif inside and out and out[-1].startswith("**") and not line.strip():
            continue  # a blank line between the heading and its first bullet
        else:
            inside = False
    return "\n".join(out)


def split(text: str, limit: int) -> list[str]:
    """Pieces of at most `limit` characters, cut at line ends where a line fits."""
    chunks, current = [], ""
    for line in text.split("\n"):
        while len(line) > limit:
            if current:
                chunks.append(current)
                current = ""
            chunks.append(line[:limit])
            line = line[limit:]
        candidate = f"{current}\n{line}" if current else line
        if len(candidate) > limit:
            chunks.append(current)
            current = line
        else:
            current = candidate
    if current or not chunks:
        chunks.append(current)
    return chunks


def messages(rel: dict) -> list[dict]:
    """The webhook payloads for one release: one embed each, the first with title and link."""
    title = f"{rel['tag_name']} is out!"[:TITLE_LIMIT]
    footer = "Pre-release" if rel.get("prerelease") else ""
    body = to_discord(changes(rel.get("body") or "")) or "Download and setup on the release page."
    # Leave room for the title and footer inside the 6000 total.
    room = min(DESCRIPTION_LIMIT, MESSAGE_LIMIT - TITLE_LIMIT - len(footer) - 32)
    parts = split(body, room)
    out = []
    for i, part in enumerate(parts):
        embed = {"description": part, "color": COLOR}
        note = footer if i == 0 else " ".join(filter(None, [footer, "(continued)"]))
        if note:
            embed["footer"] = {"text": note}
        if i == 0:
            embed["title"] = title
            embed["url"] = rel["html_url"]
        if i == len(parts) - 1 and rel.get("published_at"):
            embed["timestamp"] = rel["published_at"]
        out.append({"embeds": [embed], "allowed_mentions": {"parse": []}})
    return out


def backfill_order(releases: list[dict], include_pre: bool) -> list[dict]:
    published = [r for r in releases if not r.get("draft") and r.get("published_at")]
    if not include_pre:
        published = [r for r in published if not r.get("prerelease")]
    return sorted(published, key=lambda r: r["published_at"])


def github(path: str):
    request = urllib.request.Request(
        f"https://api.github.com/repos/{os.environ['GITHUB_REPOSITORY']}/{path}",
        headers={"Accept": "application/vnd.github+json",
                 "Authorization": f"Bearer {os.environ['GITHUB_TOKEN']}"})
    with urllib.request.urlopen(request) as response:
        return json.load(response)


def all_releases() -> list[dict]:
    releases, page = [], 1
    while True:
        batch = github(f"releases?per_page=100&page={page}")
        releases += batch
        if len(batch) < 100:
            return releases
        page += 1


def post(payload: dict) -> None:
    data = json.dumps(payload).encode()
    for _ in range(5):
        request = urllib.request.Request(
            os.environ["DISCORD_WEBHOOK"] + "?wait=true", data=data, method="POST",
            headers={"Content-Type": "application/json", "User-Agent": "release-notes (github actions)"})
        try:
            with urllib.request.urlopen(request):
                return
        except urllib.error.HTTPError as error:
            if error.code != 429:
                # Discord's reason only, never the URL (it holds the webhook's token).
                raise SystemExit(f"Discord refused the message: HTTP {error.code} {error.read()[:300]!r}")
            time.sleep(float(json.load(error).get("retry_after", 2)) + 0.5)
    raise SystemExit("Discord kept rate-limiting the webhook")


def send(rel: dict) -> None:
    for payload in messages(rel):
        post(payload)
        time.sleep(1)  # well under the webhook rate limit
    print(f"Posted {rel['tag_name']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="mode", required=True)
    sub.add_parser("release").add_argument("event")
    sub.add_parser("tag").add_argument("tag")
    sub.add_parser("backfill").add_argument("--no-pre", action="store_true")
    args = parser.parse_args()
    if not os.environ.get("DISCORD_WEBHOOK"):
        print("DISCORD_WEBHOOK is not set", file=sys.stderr)
        return 1
    if args.mode == "release":
        with open(args.event, encoding="utf-8") as f:
            rel = json.load(f)["release"]
        if rel.get("draft"):
            print("Draft release, not posted")
            return 0
        send(rel)
    elif args.mode == "tag":
        send(github(f"releases/tags/{args.tag}"))
    else:
        for rel in backfill_order(all_releases(), not args.no_pre):
            send(rel)
    return 0


if __name__ == "__main__":
    sys.exit(main())
