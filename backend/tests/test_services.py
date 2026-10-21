"""Service-level tests for bb_dummy_server: start it on a free port and talk HTTP to it."""

import base64
import json
import os
import re
import socket
import subprocess
import sys
import time
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "bb_dummy_server.py")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ServiceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, "-u", SERVER, "--address", "127.0.0.1", "--port", str(cls.port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                urllib.request.urlopen("http://127.0.0.1:{}/status".format(cls.port), timeout=1)
                return
            except OSError:
                time.sleep(0.1)
        cls.proc.kill()
        raise RuntimeError("server did not start")

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        cls.proc.wait()

    def post(self, path, body):
        req = urllib.request.Request("http://127.0.0.1:{}{}".format(self.port, path),
                                     data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read())

    def sync(self, user_id, count):
        return self.post("/basic_utils/sync_chara_id?user_id={}".format(user_id),
                         {"CharaIdNum": count, "MessageId": "SyncCharaIdRequest",
                          "SessionId": "s", "UserId": user_id})

    # A save that meets a new account asks for fresh character ids. Without them in
    # PublishCharacterIdList the game shows "Failed to reset online character data" forever.
    def test_sync_chara_id_publishes_requested_ids(self):
        resp = self.sync(3841171, 1)
        self.assertEqual(resp["MessageId"], "SyncCharaIdResponse")
        self.assertEqual(resp["ResKind"], 0)
        ids = resp["PublishCharacterIdList"]
        self.assertEqual(len(ids), 1)
        self.assertIsInstance(ids[0]["PublishCharaId"], int)

    def test_sync_chara_id_is_stable_and_unique_per_user(self):
        a1 = [e["PublishCharaId"] for e in self.sync(1001, 2)["PublishCharacterIdList"]]
        a2 = [e["PublishCharaId"] for e in self.sync(1001, 2)["PublishCharacterIdList"]]
        b = [e["PublishCharaId"] for e in self.sync(1002, 2)["PublishCharacterIdList"]]
        self.assertEqual(a1, a2)
        self.assertEqual(len(set(a1)), 2)
        self.assertFalse(set(a1) & set(b))

    # A standard chalice's ritual asks for the one dungeon the server keeps for that chalice
    # (ChannelSearch, FixedOrGeneral 2, GetCount 1) and gives up without one: "Failed to
    # contribute ritual materials", then back to the title (rig 2026-10-17).
    PTHUMERU_SEARCH = {"FixedOrGeneralList": [{"FixedOrGeneral": 2}], "FormDataVersion": 0,
                       "GetCount": 1, "HolyGrailTypeId": 0, "MessageId": "ChannelSearchRequest",
                       "RitualLevel": 1, "SessionId": "s", "Status": 1, "SubFeatureFlag": 256,
                       "UnlockedFlagList": [{"UnlockedFlag": 0}, {"UnlockedFlag": 1},
                                            {"UnlockedFlag": 0}], "UserId": 1}
    # The Pthumeru Chalice's dungeon as the game uploads dungeons (format 5, 140 bytes, little
    # endian): depth 1 in Pthumeru, layout 0, rites 90, the chalice (goods 6109), its layers
    # (ids 9910030/35/50 with their codes), conductor "Hunter". A join through Chalice Dungeon
    # Search reads only this (rig 2026-10-18: the old layout showed Depth 0 and died in the load).
    PTHUMERU_FORM = ("050000008c000000005a0a1ddd170000" "0e379700ffffffff1337970023000000"
                     "2237970032000000" + "ff" * 48 + "48756e74657200000000000000000000"
                     "480075006e007400650072" + "00" * 25)

    def test_standard_chalice_search_finds_its_dungeon(self):
        found = self.post("/channel/search?user_id=1", self.PTHUMERU_SEARCH)["ChannelList"]
        self.assertEqual(len(found), 1)
        dungeon = found[0]
        self.assertEqual(base64.b64decode(dungeon["FormData"]).hex(), self.PTHUMERU_FORM)
        self.assertEqual((dungeon["RitualLevel"], dungeon["HolyGrailTypeId"],
                          dungeon["FixedOrGeneral"], dungeon["Status"]), (1, 0, 2, 1))

    def test_standard_chalices_build_their_story_layouts(self):
        # A ritual builds its dungeon from the record's rites byte (FormData byte 9), even with the
        # chalice in hand: rites 0 gave the story Pthumeru Chalice the root layout m29_10_00_00
        # and its boss the root loot; 90 opens m29_10_90_00 (rig BB_FILE_TRACE, 2026-10-21).
        # Every standard chalice has its one m29_<key>_90_00 in the game's event folder.
        for key in (10, 20, 21, 30, 31, 40, 42, 50, 52, 53):
            search = dict(self.PTHUMERU_SEARCH, RitualLevel=key // 10, HolyGrailTypeId=key % 10)
            found = self.post("/channel/search?user_id=1", search)["ChannelList"]
            self.assertEqual(len(found), 1, key)
            form = base64.b64decode(found[0]["FormData"])
            self.assertEqual((form[8], form[9], form[10]), (0, 90, key), key)

    def test_standard_chalice_dungeon_details(self):
        found = self.post("/channel/search?user_id=1", self.PTHUMERU_SEARCH)["ChannelList"]
        details = self.post("/channel/get_details_info?user_id=1",
                            {"ChannelIdList": [{"ChannelId": found[0]["ChannelId"]}],
                             "MessageId": "ChannelGetDetailsInfoRequest", "SessionId": "s",
                             "UserId": 1})
        self.assertEqual(details["ChannelList"], found)
        self.assertEqual(details["LostChannelIdList"], [])

    def test_other_searches_find_no_made_up_dungeon(self):
        # Chalice Dungeon Search (FixedOrGeneral 1, other players' dungeons) and an unknown
        # chalice stay empty.
        other = dict(self.PTHUMERU_SEARCH, FixedOrGeneralList=[{"FixedOrGeneral": 1}])
        self.assertEqual(self.post("/channel/search?user_id=1", other).get("ChannelList", []), [])
        unknown = dict(self.PTHUMERU_SEARCH, RitualLevel=9)
        self.assertEqual(self.post("/channel/search?user_id=1", unknown).get("ChannelList", []), [])


class ChaliceDungeonSearchTest(ServiceTest):
    """Chalice Dungeon Search (FixedOrGeneral 0 and 2, up to 100) lists the dungeons in use on
    this server: a teammate joins one there, free, instead of conducting its ritual. Its own
    server: what is in use depends on the requests before. Tests run in name order."""

    SEARCH = {"FixedOrGeneralList": [{"FixedOrGeneral": 0}, {"FixedOrGeneral": 2}],
              "FormDataVersion": 0, "GetCount": 100, "HolyGrailTypeId": None,
              "MessageId": "ChannelSearchRequest", "RitualLevel": None, "SessionId": "s",
              "Status": 1, "SubFeatureFlag": None,
              "UnlockedFlagList": [{"UnlockedFlag": 0}, {"UnlockedFlag": 1}, {"UnlockedFlag": 0}],
              "UserId": 2}
    UPLOAD = {"CharaId": 61458737, "FixedOrGeneral": 1, "FormData": "BQAAAIwAAAA=",
              "FormDataVersion": 0, "HolyGrailTypeId": 0, "MessageId": "ChannelUploadRequest",
              "RitualLevel": 1, "SessionId": "s", "ShareLevel": 0, "Status": 1,
              "SubFeatureFlag": 0, "UnlockFlagList": [{"UnlockFlag": 0}], "UserId": 1}

    def search(self, **changes):
        return self.post("/channel/search?user_id=2",
                         dict(self.SEARCH, **changes)).get("ChannelList", [])

    def test_a_nothing_in_use_finds_nothing(self):
        self.assertEqual(self.search(), [])

    def test_b_a_ritual_puts_its_dungeon_in_the_search(self):
        ritual = self.post("/channel/search?user_id=1", ServiceTest.PTHUMERU_SEARCH)["ChannelList"]
        self.assertEqual(self.search(), ritual)
        self.assertEqual(self.search(RitualLevel=1, HolyGrailTypeId=0), ritual)
        self.assertEqual(self.search(RitualLevel=2), [], "another depth")
        self.assertEqual(self.search(HolyGrailTypeId=1), [], "another area")

    def test_c_an_upload_is_found_and_joined_without_a_store(self):
        uploaded = self.post("/channel/upload?user_id=1", self.UPLOAD)
        channel = uploaded["ChannelId"]
        found = [d for d in self.search() if d["ChannelId"] == channel]
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["FormData"], "BQAAAIwAAAA=")
        self.assertEqual(found[0]["DiscernmentWord"], uploaded["DiscernmentWord"])
        details = self.post("/channel/get_details_info?user_id=2",
                            {"ChannelIdList": [{"ChannelId": channel}],
                             "MessageId": "ChannelGetDetailsInfoRequest", "SessionId": "s",
                             "UserId": 2})
        self.assertEqual(details["ChannelList"], found)
        self.assertEqual(details["LostChannelIdList"], [])


class SsInfoTest(unittest.TestCase):
    """ss.info drives how often the game polls for summons; retail's 30-35 s makes every
    (re)join take a minute or more, which defeats seamless auto-rejoin."""

    def fetch_ss_info(self, extra_args, host=None, address="127.0.0.1"):
        port = free_port()
        proc = subprocess.Popen(
            [sys.executable, "-u", SERVER, "--address", address, "--port", str(port)] + extra_args,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.time() + 10
            while True:
                try:
                    request = urllib.request.Request("http://127.0.0.1:{}/bb-eu/ss.info".format(port))
                    if host is not None:
                        request.add_header("Host", host)
                    with urllib.request.urlopen(request, timeout=1) as r:
                        return base64.b64decode(r.read()).decode("utf-8")
                except OSError:
                    if time.time() > deadline:
                        raise
                    time.sleep(0.1)
        finally:
            proc.kill()
            proc.wait()

    @staticmethod
    def tag(xml, name):
        match = re.search(r"<{0}>([^<]*)</{0}>".format(name), xml)
        return match.group(1) if match else None

    def test_summon_polling_is_fast_by_default(self):
        xml = self.fetch_ss_info([])
        self.assertEqual(self.tag(xml, "SummonDataCreateInterval2"), "5")
        self.assertEqual(self.tag(xml, "SummonDataGetListInterval2"), "5")

    def test_coop_guard_timers_do_not_stall_rejoin(self):
        # After a session ends the host waits out these guard timers (retail 60 s) before it
        # takes a new sign, so a guest who died could not rejoin for over a minute.
        xml = self.fetch_ss_info([])
        self.assertEqual(self.tag(xml, "SummonDataGetListGuardTimer2"), "5")
        self.assertEqual(self.tag(xml, "SummonDataGetListCoopGuardTimerClampTime2"), "5")
        # Invasion timers are not ours to touch.
        self.assertEqual(self.tag(xml, "SummonDataGetListInvationGuardTimerClampTime2"), "0")

    def test_summon_polling_interval_is_configurable(self):
        xml = self.fetch_ss_info(["--summon-poll", "12"])
        self.assertEqual(self.tag(xml, "SummonDataCreateInterval2"), "12")
        self.assertEqual(self.tag(xml, "SummonDataGetListInterval2"), "12")
        # Everything else in the template is left alone.
        self.assertEqual(self.tag(xml, "ReloadServerStatusInfoInterval2"), "300")

    def test_each_game_is_sent_to_the_address_it_reached(self):
        # No VPN: the host's own address is a LAN one (192.168.x.x) while a guest reaches the host's
        # public address. ss.info sent the guest to the LAN address, so its game never got online
        # and the host never saw it (Discord report 2026-10-21).
        xml = self.fetch_ss_info(["--shadnet-webapi", "http://192.168.4.25:31315"],
                                 host="203.0.113.7:20443", address="192.168.4.25")
        self.assertNotIn("192.168.4.25", xml)
        self.assertIn("http://203.0.113.7:", xml)
        self.assertIn("http://203.0.113.7:31315", xml)

    def test_the_given_address_without_a_usable_host(self):
        for host in ("127.0.0.1:20443", "localhost", "bad host<x>"):
            xml = self.fetch_ss_info([], host=host, address="192.168.4.25")
            self.assertIn("http://192.168.4.25:", xml, host)


if __name__ == "__main__":
    unittest.main()
