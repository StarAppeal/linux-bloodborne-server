# Agent C: session layer, join handshake, transport, matching (static RE, 01.09)

Addresses are eboot_rel.img offsets (image base 0).

## 0. Objects (correction to the brief)
| Pointer | Object | Notes |
|---|---|---|
| [0x0553D6D0] | "NetSync" (our code: net sync object / SummonSessionRules). Built by 0x017875D0 (called from game init 0x0156C6B0), vtable 0x0532C090 | slot0 = 0x0178B5E0 OnSessionEvent(this, event*); slot1 = 0x0178D510 OnReceive(this, member*, kind, buf, len); state at +0x84; peers (recv queues) list +0x28; pending-members list +0x48 (0x30-byte nodes); member->slot list +0x68 (0x38-byte nodes, slot at +0x30); bytes sent +0x88, bytes received +0x8C; lock +0x130 |
| [0x05540290] | SprjSessionManager (FD4 singleton name) | +0x18 = session handle {impl*, mutex} (0xC8F570 "has session"); +0xD0 = host member id; +0xF8..+0x100 members (0x20 each); +0x118 closing flag; +0x124 matching state (3 hosting, 6 summoned guest); +0x188/+0x1B0 reliable resend queue (bytes); update 0x01ED0B20 |
| [0x056C6D70] | member-identity registry (NexusRevolution2) | list of known identities +0x08; +0x20 = local MemberRef (set at matching-context start from sceNpGetNpId, 0x00CBD072 -> 0xCA4500) |

The brief says "while state==4 only channels 7,8,0xA-0xD,0x10,0x11,0x19,0x42 may be sent". It is the other way round:
0x0178963B `cmp [this+0x84],4; je send` -> in state 4 every channel goes; in any other state only
channels 7,8,0x0A,0x0B,0x0C,0x0D,0x10,0x11,0x19,0x42 (mask 0x080000000004067B from 7). Receive filter
0x01790C7F is the same idea: when state != 4 unreliable channels 1,2,4,5,6,0x12,0x13,0x14 are dropped.

### NetSync state (+0x84)
| Value | Set at | Meaning |
|---|---|---|
| 0 | 0x0178A9AA (reset 0x0178A900) | no session |
| 1 | 0x0178BA7C (event 8/9 success, Lua "OnJoinMutiplay") | guest: joined host's session, waiting for host data |
| 2 | 0x01785236 (after ch 0x10 event flags applied) / 0x0178D970 | guest: host world data in, summon reload starting (EventMan 0x01317780) |
| 3 | 0x0178D9A0 | guest: reload/loading (setter; caller not traced) |
| 4 | 0x0178BDDB (event 7 success = host session created) / 0x0178D9E3 (0x0178D9C0, called by world load 0x01939470, also sends ch 0x0A to pending members) | in world, full traffic |

## 1. Session events -> 0x0178B5E0 (switch on event.type, jump table 0x0178D468)
Event object accessors: 0xC8B110 type(+0), 0xC8B120 success(+4 byte), 0xC8B130 +8, 0xC8B140 +0x10, 0xC8B160 member ref (+0x20).
| type | Meaning (from code) | Action |
|---|---|---|
| 2 | ? | GameMan(0x5556678)+0x1590 -> this+0xBD=1 |
| 5,6 | ? | flags this+0xBE, walk list this+0xB0 |
| 7 | CreateSession result (host) | ok: +0x110=0, state=4, clear slot list, reset GameDataMan remote slots, local join task kind arg 3 (0x01E4F9F0/0x01E55A70). fail: reset, retry count +0x110, "OnFailedCreateSession" after 4 |
| 8,9 | JoinSession result (guest) | ok: state=1, reset remote slots, Lua "OnJoinMutiplay", player+0x3D8 = local member, local join task arg 0. fail: count +0x114, "OnFailedJoinSession" after 4 |
| 0xC | session gone | if state != 3: reset (0x0178A900) |
| 0xE | MEMBER JOINED (ev member) | skip self; join task (arg 2 if member == SSM+0xD0 host, else arg 1); add to pending list +0x48; take free GameDataMan remote slot (4 max) -> list +0x68; create peer recv queues (0x017906E0, 0x88 bytes) in +0x28. If SSM+0x124 == 3 (we host): send ch 7, ch 0x19, then 0x01786050 (ch 0x10 event flags 'MNSE' deflate + ch 0x11 world blobs, guess), 0x01786360 (WorldDecalMan), bullets 0x01A04190; NetWorldChrSync owner 0x018F0DB0(WorldChrMan+0xA40, member). Everyone: send ch 0x42. Then summon-sign bookkeeping by player name (0x018777E0) |
| 0xF | MEMBER LEFT | 0x01877F50 (sign list), delete peer queues, SSM 0x01ED2650(member) (drop its resend queue), free GameDataMan slot, ... |
| 0x15-0x17 | forwarded to FrpgNetMan+0xC98 -> +0x48 (0x014AC120) | ? (voice/lobby, guess) |
| 0x1E | kicked / room closed | if state != 3: GameMan+0x1592 ? "OnJoinClearedRoom" : "OnBeKickOut"; 0x014EB860; MultiPlayStopRequest 0x01ED07A0 |

## 2. Join handshake channels
| ch | size | dir | payload | receiver |
|---|---|---|---|---|
| 7 | 4 | host -> new member (on event 0xE, only if SSM+0x124==3) | u32 bool: FrpgNetMan(0x553B120)+0xA98==2 ? 1 : (+0xB14 & 1) | no recv site found in recv_sites.txt (read elsewhere, TODO) |
| 0x19 | 0x44 | host -> new member | u32 = GameDataMan(0x553B130)+0x68 (rest of 0x44 bytes is stack junk) | guest in state 1 (0x01784E16): GameMan+0x1593=1, +0x1594 = value |
| 0x42 | 1 | every member -> new member | GameMan+0x16F4 (own player slot/number) | 0x01784CCF (per member): that member's join task +0xEC = byte, +0xED |= 4 (and bit 5 from FrpgNetMan flag) |
| 0x11 | <=0x6400 | host -> guest | 3 length-prefixed blobs | guest state 1: GameMan +0x16C0/0x16C8, +0x16A0/0x16A8, +0x1684/0x1688 |
| 0x10 | <=0x800 | host -> guest | 'MNSE' magic, u16 ver 1, sizes, deflate (level 9) event flags | guest when SSM+0x124==6 and state 1: SprjEventFlagMan apply 0x013BECA0, state=2, start reload |
| 0x0A | 1 | any | "send me your data" (see brief) | |

## 3. Send/receive path below the packet layer
Send 0x017894E0(netsync, member, ch, buf, n): needs SSM session + >=2 members + member valid (0xCA1400) + not self; packet {u32 ch, u32 n, payload} in static 0x055640F0 (max 0x6400);
-> 0xC8F0A0(SSM+0x18, reliable = ch>6, member, pkt, n+8) -> impl 0xC976C0: picks channel config (vector impl+0x2D8, 0xD8 each, id 0 unreliable / 1 reliable)
-> transport mgr (singleton 0x056C6E50) 0xCB2D20: find socket group by vport (+0x60 u16), then the connection whose identity (+0x40) equals the member identity, queue send request 0xCCAEA0(mgr+0x28, {sock+0x18, conn+0x18, cfg type, buf, n}).
If the send fails and ch>=7: copy goes to SSM resend queue 0x01ED2310 (resent by 0x01ED0B20); if >1000 bytes are queued -> Lua "OnIrregularLeaveSession" + MultiPlayStopRequest.

Sockets (NexusRevolution2 "ExtNetwork2"): 0x02719430 creates "extnetwork2_udpp2p_ipv4_%d" = sceNetSocket(AF_INET, SOCK_DGRAM_P2P(6)), bind port +0x30 vport +0x32; 0x02718E80 plain UDP "extnetwork2_udp_ipv4_%d". Send 0x02719360 sceNetSendto(addr, port, vport). Recv 0x02719580: up to 21 sceNetRecvfrom per call, each -> 0x02716CD0.
Demux 0x02716CD0: hash (ip, port|vport<<16) -> connection; unknown address + first byte <=3 -> accept callback creates a connection. Per connection 0x0271AA60: datagram = records [u8 kind 0..3][u16 BE len][payload]; kinds 0..3 = 4 sub-streams at conn +0x68/+0x70/+0x80/+0xA0; odd kinds validated by 0x0271DCB0 (reliable header, guess); stream callback 0x0271C7B0 -> up to the session -> NetSync slot1 0x0178D510.
NetSync 0x0178D510 -> peer for member -> 0x01790C20: kind 0 (unreliable): one datagram = one {ch,size,payload} packet, ch<=0x43, queue capped at [0x051282CC] entries (oldest dropped); kind 1 (reliable stream): reassembles {ch,size} headers across chunks (peer +0x50..+0x78) then queues. Queues: peer +0x30 = 0x44 lists; pop 0x017910F0.

Member id = MemberRef {identity*, lock} (0x20 in lists). Identity = NexusRevolution player id object (vtable 0x052C5C00, 0x60 bytes): +0x0C isLocal, +0x30 u32 + +0x34 u16 alt key, +0x36 SceNpId (36 bytes). Equality 0xCA29A0: sceNpCmpNpId, else (u32,u16) pair. Built from NpId (0xCA22F0) or from (u32,u16) of a signaling/room record (0xCA23B0, from 0x02378C30 [+4] / 0x02378C50 [+0xE]). So peers are addressed by NpId, not room member id; the transport maps identity -> connection.

## 4. NP matching layer (NexusRevolution2 Ps4NpSessionLight)
Context start 0x00CBCF70: sceNpGetNpId -> local identity; sceNpMatching2CreateContext/ContextStart/GetServerId; request opt timeout 30 s (cb 0x00CBD4A0); callbacks: room 0x00CBD540 -> 0x00CC0170, signaling 0x00CBD560 (queues {ctx, event, roomId, memberId, error, time} to [arg+0xA8]), lobby 0x00CBD720, context 0x00CBD310.
Thin import wrappers: JoinLobby 0xCC7480, LeaveLobby 0xCC74F0, ContextStop 0xCC7560, SearchRoom 0xCC7620, GetWorldInfoList 0xCC7700, CreateJoinRoom 0xCC77E0, JoinRoom 0xCC7850, LeaveRoom 0xCC78C0, KickoutRoomMember 0xCC7930, GrantRoomOwner 0xCC79A0, SetRoomDataInternal 0xCC7A10, SetRoomDataExternal 0xCC7A80, SetRoomMemberDataInternal 0xCC7AF0, SignalingGetConnectionStatus 0xCC7B60. NpSignaling: init 0xCC0800, Activate/Deactivate connection 0xCD8BB0, 0xCD86C0, 0xCDA0B0.
sceNpWebApi is only PSN social: friend list 0xCB6660 (/v1/users/%s/friendList), block list 0xCB71D0, activity feed 0xCB7A90 (PLAYED_WITH). The FromSoft game server is plain sceHttp through FD4HttpMan: create 0x00FD2740 (sceHttpCreateConnectionWithURL/CreateRequestWithURL), send 0x00FD2970; address from "testdata:/FrontServerLeaf_sp.address" (0x01ECE640). The <api_*> strings (0x04938448..) are XML config tags, not URLs; the paths seen on the wire are /summon_messenger/{create,get,request,delete} with JSON bodies (SummonDataCreate/GetList/Summon/Remove).

## 5. Summon flow (game level)
- Session create/join go through NetSync -> SprjSessionManager -> NR session (0xC8xxxx) -> NpMatching2 room:
  - Host: summon manager QueueInsert 0x01875320 (also Condition.SOS 0x0130C590) -> 0x01876060 -> NetSync CreateSession 0x01788ED0 (another copy is 0x0178E400, which falls back to JoinSession at 0x0178E8E4) -> SSM 0x01ECFCD0. That builds the channel configs (0xC83C00 + 0x017905E0, repeated; 0xD8 each) and then the NR create (CreateJoinRoom wrapper 0xCC77E0, reached through NR vtables 0xCC48D0/0xCD5880/0xCD6450). Result = session event 7.
  - Guest: summon manager update 0x01872360 at 0x018738BB. The condition is SSM+0x124 in {0,2,5}, and the queue entry +0x1F8 says it was summoned. The update then parses the summon blob (entry +0xB8 ptr, +0xB0 len): first u32 -> GameMan(0x5556678)+0x16F0, next u8 -> GameMan+0x16F4 (that byte is what this game later sends on ch 0x42 = its player slot). The rest is session join data parsed by 0xC90DD0 into a join param (0xC8FBF0). It then calls NetSync JoinSession 0x01789010 -> SSM 0x01ED0670 -> NR join (JoinRoom wrapper 0xCC7850). The summon position (entry +0x20..+0x48) goes to GameMan+0x14A0/+0x14C0. Result = session event 8/9 (Lua "OnFailedJoinSession" after 4 failures).
  - The summon manager also broadcasts ch 0x32 (0x10 bytes) to all members (0x01872E78; not analysed).
- Server side (shadNet, ours): POST /summon_messenger/create = SummonDataCreate (the sign; re-posted about every 30 s). /get = SummonDataGetList (host search, every 5 s while the bell is on). /request = SummonDataSummon: the host claims a sign, and its body (with the host's session data) is handed to the sign owner as the response of that owner's next /create (BuildClaimDeliveryResponse, bloodborne_summon_broker.cpp:710). /delete = SummonDataRemove. Fields: SessionId, UserId, SummonData blob, SummonDataVersion 3, SummonType(List), AreaId/AreaRegionId/ChannelId, MatchingLevel, PosX/Y/Z, SummonWord. The emulator adds the Party/Host-Placement/Seamless headers (emulator http.cpp:807).
- The P2P link per peer is the NR connection state machine 0x00CD8BB0 (state +0xB4, 0..9): sceNpLookupNpId (online id -> NpId), then sceNpSignalingActivateConnection(NpId), then polling sceNpSignalingGetConnectionStatus, and Deactivate on failure. Matching2 signaling events go to the queue at 0x00CBD560. Our p2p_sockets.cpp notes that about 1 join in 4 stalls with this machine giving up 5.4 s after signaling came up.

## 6. Leave, kick and timeout
- Per frame, NetSync update 0x01789B60 runs: receive/apply 0x01783160, 0x01780960, then the SSM update 0x01ED0B20 (reliable resend).
- Lua "OnIrregularLeaveSession" is raised in three cases:
  - the SSM timer +0x2E0 exceeds its limit +0x2F0 (0x01789C4D, no traffic: timeout);
  - the receive queues overflow (0x0178A367);
  - the resend queue holds more than 1000 bytes (0x017897D0).
  Each case is followed by MultiPlayStopRequest 0x01ED07A0 and 0x01ED2710.
- Event 0x1E: Lua "OnBeKickOut" (or "OnJoinClearedRoom" when GameMan+0x1592 is set), then a stop request.
- Event 0xF: member left. Its queues and remote slot are freed.
- Kick-all: ForceSummonSuccess 0x013849A0 -> 0x0178D7B0. It sets SSM+0x118 = 1 and kicks every pending member via 0xC8EE30.
- OnP2PTimeOut, OnSummonTimeOut and OnIrregularLeaveSession_1 only appear in the Lua event-name table 0x0138D380 (OnIrregularLeaveSession also at 0x01316180, the _1 variant at 0x01384050). The code that raises OnP2PTimeOut was NOT found (it is probably raised by id).
- Playlog markers (Host_SendP2PhandShakeRequest / RejectedP2PhandShake / SessionTimeout) are in 0x01EB65E0, which is only logging.
- Steps: FrpgNetConnectManStep 0x0147CA20, FrpgNetLobbyStep 0x01498860, CSRequestSummonStep 0x014BB4C0 (Init/Update 0x014B47C0/0x014B4890), CSSessionConnectStateStep 0x01ED2BE0. Of these, only the names were mapped.

## 7. What already runs on our code
| Layer | Game code (runs unchanged) | Ours |
|---|---|---|
| Game server | FD4HttpMan sceHttp | emulator http.cpp routes /summon_messenger/* to shadNet; webapi_routes_bloodborne.cpp + SummonBroker |
| Rooms | NR wrappers 0xCC77E0 etc. | emulator np_matching2*.cpp -> shadNet TCP (Create/Join/Leave/Kickout, RoomEvent 0x1101/0x1102/0x1103/0x1106) |
| Signaling | NR conn machine 0xCD8BB0, cb 0xCBD560 | np_matching2_signaling.cpp: own UDP handshake Offer/Accept/Check (0x32 bytes, retry 350 ms, timeout 10 s) raising ESTABLISHED/DEAD; np_signaling.cpp Activate/GetConnectionStatus; STUN on shadNet UDP 31314 |
| P2P socket | ExtNetwork2 udpp2p 0x02719430 | p2p_sockets.cpp: one real UDP socket, each frame prefixed {u16 src vport, u16 dst vport} (:365), demuxed by dst vport (:580) |
| Reliability, demux, channels, join handshake | ExtNetwork2 records + streams, NetSync {ch,size}, per-member queues, ch 7/0x19/0x10/0x11/0x42 | nothing; all game code. Our co-op frames (SHAD, PeerHello) go beside it to room peers |

## 8. Sequence: host summons guest
```
Guest game                 shadNet HTTP           Host game                     shadNet rooms
 sign: POST /create  ----> store sign
                           <---- POST /get (every 5 s, bell on)
                           ---- SummonDataList ---> QueueInsert 0x01875320 -> 0x01876060
                                                   NetSync CreateSession 0x01788ED0 -> SSM 0x01ECFCD0
                                                   -> CreateJoinRoom -------------------> room
                                                   event 7 ok: state=4, local task (arg 3)
                           <---- POST /request (claim + host session data)
 POST /create (re-post) -> claim in the reply
 0x01872360: blob u32->GameMan+0x16F0, u8 slot->+0x16F4, join data 0xC90DD0
 NetSync JoinSession 0x01789010 -> SSM 0x01ED0670 -> JoinRoom -----------------------> MEMBER_JOINED
 0xCD8BB0: NpLookupNpId -> NpSignalingActivateConnection   <==== UDP P2P (vport) ====>
 event 8/9 ok: state=1, OnJoinMutiplay                    event 0xE: join task (arg 1), pending +0x48,
                                                          remote slot, peer queues
           <== ch 7 (u32 flag)  ch 0x19 (u32 GameDataMan+0x68)   [only if SSM+0x124==3]
           <== ch 0x11 (3 blobs)  ch 0x10 ('MNSE' deflate flags)  decals, bullets, enemy owner 0x018F0DB0
 ch 0x42 (my slot) ==>                               <== ch 0x42 (host slot)
 0x01784E16: 0x19 -> GameMan+0x1593/94; 0x11 -> GameMan+0x16C0/0x16A0/0x1684
 SSM+0x124==6: 0x10 -> SprjEventFlagMan, state=2 -> summon reload (0x01317780); state 3 while loading
 world load 0x01939470 -> 0x0178D9C0: state=4, ch 0x0A to pending members
 ch 8/0xB/0xC/0xD/0x13/0x3B/0x3C character data (0x01781AC0) <==> both ways; tasks reach steps 4/5/6
```

## 9. Team join (phase 6, no bell, no signs)

Traced on the rig (2026-10-14): when a summon goes out, the host's summon manager update
(0x01872360) writes each guest a packet of `{u32 0xFFFFFFFF, u32 7}` and the session's join data,
and sends it over vport 40 (0x014BAEC0(member, len, data), 271 bytes with the transport's framing).
The join data is the game's own (193 bytes, starting "NXRV"), the same for every guest; the two
words become the guest's GameMan +0x16F0 (the summon's kind for the Lua events) and +0x16F4 (the
sign's type, sent on ch 0x42).

The port does the same without a summon (bloodborne_join.cpp):
- **Host** (`OpenTeamSession`, `TeamSessionJoinData`): NetSync CreateSession 0x01788ED0(NetSync,
  55.0f) as 0x01876060 calls it; once hosting (SSM +0x124 == 3): SessionInfo 0x00C8F500(SSM+0x18,
  SSM+0x38, 1), the join param copied from it 0x00C90270, checked 0x00C90D80, written by
  0x00C90B00 into the game's memory writer (vtable 0x053B2C80; +0x10 buffer, +0x18/+0x38 capacity,
  +0x30 bytes written), freed 0x00C90520.
- **Guest** (`JoinTeamSession`): the param made 0x00C8FBF0 and read 0x00C90DD0 from the game's
  memory reader (vtable 0x053B2BE0; +0x08 size, +0x10 data, +0x18 position, +0x24 = 1), GameMan
  +0x16F0/+0x16F4 set, NetSync JoinSession 0x01789010(NetSync, param). No summon placement and
  no reload: when the host's world comes in (state 2, 0x01785236) the join's start event
  0x01317780 (Lua "OnBeJoinStart_*", which starts the summon reload) is skipped, and on the next
  tick 0x0178D9C0 (what the world load's end 0x01939470 calls after a summon reload) finishes the
  join: state 4, ch 0x0A to the members. The guest stays in its world, which took the team's
  world as its save loaded.
- **Not from the title:** NetSync JoinSession asserts the map's event object ([0x0553B0C8],
  made with the world); joining through the session manager's own JoinSession (0x01ED0670)
  joined the room but NetSync never left state 0 and the game left the room after 20 s: the
  game's session layer runs only with a world.
- The server carries the join data between them (`bbcoop::TeamSession`).
