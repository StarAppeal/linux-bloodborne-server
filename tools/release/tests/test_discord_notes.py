# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for discord_notes.py: turning a GitHub release into Discord webhook messages.

Run: py -3.12 -m unittest discover -s tools\\release\\tests
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import discord_notes as tool  # noqa: E402


def release(body="**What's new since v1.2.2**\n- A fix", **extra):
    data = {
        "tag_name": "v1.2.3",
        "name": "Yharnam Together v1.2.3",
        "html_url": "https://github.com/o/r/releases/tag/v1.2.3",
        "published_at": "2026-10-22T18:12:00Z",
        "draft": False,
        "prerelease": False,
        "body": body,
    }
    data.update(extra)
    return data


class MarkdownTest(unittest.TestCase):
    def test_headings_become_bold_lines(self):
        self.assertEqual(tool.to_discord("## What's new\ntext"), "**What's new**\ntext")

    def test_html_comments_are_dropped(self):
        self.assertEqual(tool.to_discord("a<!-- hidden -->b"), "ab")

    def test_blank_line_runs_collapse(self):
        self.assertEqual(tool.to_discord("a\n\n\n\nb\n"), "a\n\nb")

    def test_windows_line_ends(self):
        self.assertEqual(tool.to_discord("a\r\nb"), "a\nb")


V114 = """**What's new since v1.1.3**
- The Impurity rune now works in co-op.
- Fog walls are off on every map.

Everyone, the host included, must update to v1.1.4.

---

Seamless co-op for Bloodborne on Bloodborne PC.

1. Download `YharnamTogether-v1.1.4-setup.exe` and run it.

Built from 0758ae9f. SHA-256 of the setup: `cade`
"""

V011 = """Seamless co-op for Bloodborne.

### Getting started
1. Download the zip.
   - everyone runs `1 - SETUP`;

### Fixed in v0.1.1
- **Portraits now show.** Replace v0.1.0.

### What's in this release
- **Seamless co-op.** Join each other.
- **Party death is always on**:
  - A killing blow puts you down.

### Known gaps
- Joining occasionally stalls.

Built from 2a89c38f1e3f (main).
"""


class ChangesTest(unittest.TestCase):
    def test_only_the_change_list_stays(self):
        self.assertEqual(tool.changes(V114),
                         "**What's new**\n- The Impurity rune now works in co-op.\n- Fog walls are off on every map.")

    def test_old_layout_keeps_fixed_and_whats_in_sections(self):
        self.assertEqual(tool.changes(V011),
                         "**Fixed in v0.1.1**\n- **Portraits now show.** Replace v0.1.0.\n\n"
                         "**What's in this release**\n- **Seamless co-op.** Join each other.\n"
                         "- **Party death is always on**:\n  - A killing blow puts you down.")

    def test_notes_without_changes_are_empty(self):
        self.assertEqual(tool.changes("Seamless co-op.\n\n1. Download the zip."), "")


class SplitTest(unittest.TestCase):
    def test_short_text_is_one_chunk(self):
        self.assertEqual(tool.split("hello", 100), ["hello"])

    def test_splits_on_line_ends_under_the_limit(self):
        text = "\n".join(f"- line {i:03}" for i in range(100))
        chunks = tool.split(text, 200)
        self.assertGreater(len(chunks), 1)
        self.assertTrue(all(len(c) <= 200 for c in chunks))
        self.assertEqual("\n".join(chunks), text)

    def test_hard_splits_a_line_longer_than_the_limit(self):
        chunks = tool.split("x" * 450, 200)
        self.assertEqual([len(c) for c in chunks], [200, 200, 50])


class MessagesTest(unittest.TestCase):
    def test_one_embed_with_title_link_and_notes(self):
        [message] = tool.messages(release())
        [embed] = message["embeds"]
        self.assertEqual(embed["title"], "v1.2.3 is out!")
        self.assertEqual(embed["url"], "https://github.com/o/r/releases/tag/v1.2.3")
        self.assertEqual(embed["description"], "**What's new**\n- A fix")
        self.assertEqual(embed["timestamp"], "2026-10-22T18:12:00Z")
        self.assertEqual(message["allowed_mentions"], {"parse": []})
        self.assertNotIn("footer", embed)

    def test_pre_release_is_marked(self):
        [message] = tool.messages(release(prerelease=True))
        self.assertIn("Pre-release", message["embeds"][0]["footer"]["text"])

    def test_full_release_notes_post_only_the_changes(self):
        [message] = tool.messages(release(body=V114, tag_name="v1.1.4"))
        embed = message["embeds"][0]
        self.assertEqual(embed["title"], "v1.1.4 is out!")
        self.assertNotIn("Download", embed["description"])
        self.assertNotIn("must update", embed["description"])

    def test_long_notes_split_within_discord_limits(self):
        body = "**What's new**\n" + "\n".join(f"- change number {i} " + "y" * 60 for i in range(400))
        sent = tool.messages(release(body=body))
        self.assertGreater(len(sent), 1)
        for message in sent:
            for embed in message["embeds"]:
                self.assertLessEqual(len(embed["description"]), tool.DESCRIPTION_LIMIT)
                self.assertLessEqual(len(embed.get("title", "")), tool.TITLE_LIMIT)
            total = sum(len(e["description"]) + len(e.get("title", "")) + len(e.get("footer", {}).get("text", ""))
                        for e in message["embeds"])
            self.assertLessEqual(total, tool.MESSAGE_LIMIT)
        self.assertIn("(continued)", sent[1]["embeds"][0]["footer"]["text"])
        self.assertNotIn("title", sent[1]["embeds"][0])

    def test_empty_notes(self):
        [message] = tool.messages(release(body=None))
        self.assertEqual(message["embeds"][0]["description"], "Download and setup on the release page.")


class BackfillOrderTest(unittest.TestCase):
    def test_oldest_first_without_drafts(self):
        releases = [
            release(tag_name="v3", published_at="2026-10-03T00:00:00Z"),
            release(tag_name="v2", published_at=None, draft=True),
            release(tag_name="v1", published_at="2026-10-01T00:00:00Z", prerelease=True),
        ]
        self.assertEqual([r["tag_name"] for r in tool.backfill_order(releases, True)], ["v1", "v3"])
        self.assertEqual([r["tag_name"] for r in tool.backfill_order(releases, False)], ["v3"])


if __name__ == "__main__":
    unittest.main()
