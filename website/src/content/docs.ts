// The documentation, as data. The pages render it, and /llms-full.txt prints the same text as
// Markdown, so people and machines read one source. Inline text understands `code`, **bold** and
// [links](/path).

import type { FigureId } from "@/components/figures";
import { packetRates, rateTotal, roles } from "@/content/traffic";
import { ISSUES_URL, RELEASES_URL, REPO_URL, blob, netcodeStatus } from "@/lib/site";

export type Block =
  | { type: "p"; text: string }
  | { type: "ul"; items: string[] }
  | { type: "ol"; items: string[] }
  | { type: "code"; text: string; lang?: string }
  | { type: "table"; head: string[]; rows: string[][] }
  | { type: "note"; text: string }
  | { type: "h3"; text: string }
  /** Terms and what they mean, like the game's two-column menus: a label, then its text. */
  | { type: "terms"; items: [string, string][] }
  /** A diagram or chart drawn by the site (src/components/figures.tsx); `alt` is its text
   *  version for screen readers and /llms-full.txt. */
  | { type: "figure"; figure: FigureId; caption: string; alt: string }
  /** A screenshot in public/; width and height are the file's own. */
  | {
      type: "image";
      src: string;
      alt: string;
      caption: string;
      width: number;
      height: number;
    }
  /** Several screenshots of one screen or flow, shown one at a time under a row of tabs. */
  | { type: "gallery"; items: GalleryItem[] };

export type GalleryItem = {
  /** The tab's few words. */
  label: string;
  src: string;
  alt: string;
  caption: string;
  width: number;
  height: number;
};

/** The painted icons in public/art, shown beside a section's title. */
export type IconName = "lantern" | "blades" | "bell" | "tricorn" | "vial" | "skull" | "hand";
export type Section = { id: string; title: string; icon?: IconName; blocks: Block[] };

export type DocPage = {
  slug: string;
  title: string;
  /** One or two sentences: the page's meta description and its line in /llms.txt. */
  description: string;
  sections: Section[];
};

const install: DocPage = {
  slug: "install",
  title: "Install and set up",
  description:
    "What you need, how to install Yharnam Together, what each part of the launcher's Co-op page is for, and how the other players reach the host.",
  sections: [
    {
      id: "requirements",
      title: "Before you start",
      blocks: [
        {
          type: "ul",
          items: [
            "Your own Bloodborne for PS4 at version 1.09: the game folder with the 1.09 update, or the PS4 `.pkg` files. Nothing from the game is included.",
            "Windows 10 (1903 or later) or Windows 11, 64-bit.",
            "A graphics card with Vulkan 1.3 and a current driver.",
            "Everyone, the host included, on the same release. The server turns away a different build.",
          ],
        },
        ...(netcodeStatus.shippedInRelease
          ? []
          : [{ type: "note" as const, text: netcodeStatus.note }]),
      ],
    },
    {
      id: "install",
      title: "Install",
      blocks: [
        {
          type: "ol",
          items: [
            `Download the setup \`.exe\` from the [download page](/download) or [GitHub releases](${RELEASES_URL}) and run it. It installs for your Windows user and adds a desktop shortcut.`,
            "Open **Bloodborne Seamless Co-op**. The launcher opens on **Home**, with the latest news.",
            "On first start the **Co-op** page opens by itself. Fill it in (below); **Done** lights up once nothing is missing. Then press **PLAY**.",
          ],
        },
        {
          type: "p",
          text: "Every screen of the setup and every launcher page, with what each is for, is in [Setup and the launcher](/docs/launcher).",
        },
        {
          type: "image",
          src: "/docs/launcher-home.webp",
          width: 1200,
          height: 780,
          alt: "The launcher's Home page: the news card Hunt Yharnam together, the sidebar with Home, Co-op and Settings, and the bottom bar with the status icons, Start server, Stop server and PLAY.",
          caption: "Home: the news, the sidebar and the PLAY bar.",
        },
      ],
    },
    {
      id: "coop-card",
      title: "The Co-op page",
      blocks: [
        {
          type: "p",
          text: "One page for everyone, host or not. You fill it in once.",
        },
        {
          type: "image",
          src: "/docs/launcher-coop.webp",
          width: 1200,
          height: 693,
          alt: "The Co-op page filled in: the Bloodborne folder with a green check, version 01.00 with the 1.09 update next to it, the host's IP 10.0.0.2, Enemies Off (the normal game), the Bring saves button and Done.",
          caption: "Filled in: the folder is found, the host's IP is set and Enemies is Off.",
        },
        {
          type: "terms",
          items: [
            [
              "Your Bloodborne folder",
              "The game, at 1.09 or with the 1.09 update next to it. **Browse…** picks the folder; **Find it for me** searches the usual places and then every drive; **Install from PKG…** installs the game and the update from your `.pkg` files. The line under it says which version it found, or what is missing.",
            ],
            [
              "Host's IP address",
              "Where the host's PC can be reached: its VPN address, or its public IP. The host types their own address too; that is how the launcher knows this PC is the host.",
            ],
            [
              "Enemies",
              "Off is the normal game. **Harder** gives every enemy and boss three times the HP and poise and half again the damage, to make up for the extra hunters. Everyone picks the same.",
            ],
            [
              "Bring saves from another folder…",
              "Opens a folder picker and copies your characters over from a PS4 save folder, a shadPS4 `user` folder or an older co-op install.",
            ],
            [
              "Done",
              "Greyed out until the page is complete. The line beside it says what is still needed.",
            ],
          ],
        },
        {
          type: "image",
          src: "/docs/launcher-coop-empty.webp",
          width: 1200,
          height: 693,
          alt: "The Co-op page on first start: the folder empty with the warning Choose your Bloodborne folder, the host's IP and Enemies empty, Done greyed out and the line Still needed: the game folder, the host's IP, the enemies.",
          caption:
            "On first start, Done waits until the folder, the host's IP and the enemies are set.",
        },
        {
          type: "p",
          text: "There is no account to make. The others see your character's name.",
        },
      ],
    },
    {
      id: "play",
      title: "Press PLAY",
      blocks: [
        {
          type: "ol",
          items: [
            "When the host's IP is this PC's own address, PLAY starts the co-op server first. If it ever stops, it restarts by itself.",
            "The first time, Windows asks once to let co-op through its firewall. Say yes: the launcher adds the rules for you.",
            "The first start prepares a game folder that links to your game. Nothing is copied.",
            "The game starts. Making a team happens in the game: see [Playing together](/docs/play).",
          ],
        },
        {
          type: "image",
          src: "/docs/launcher-bar.webp",
          width: 1200,
          height: 97,
          alt: "The launcher's bottom bar: four status icons, Start server, Stop server and PLAY, with the frame rate, upscaler and resolution under the icons.",
          caption: "The bottom bar.",
        },
        {
          type: "image",
          src: "/docs/launcher-server-running.webp",
          width: 1200,
          height: 97,
          alt: "The same bottom bar on the host with the server shown as Running.",
          caption: "On the host, with the server running.",
        },
        {
          type: "terms",
          items: [
            [
              "The four icons",
              "Quick checks of your setup. A green check means ready, a grey dot means it is not set yet.",
            ],
            [
              "Start server, Stop server",
              "For running the server without the game, for example when the host plays later. The word beside them says whether it is running.",
            ],
            [
              "The line under them",
              "Your frame rate, upscaler, resolution and mode, or the last thing the launcher did.",
            ],
          ],
        },
        {
          type: "p",
          text: "If the game crashes, the next PLAY sends the crash report and the logs to the host's server, so the host can pass them on.",
        },
      ],
    },
    {
      id: "network",
      title: "Reach the host",
      blocks: [
        {
          type: "p",
          text: "The host's PC is the server, so every other player has to reach it. Either:",
        },
        {
          type: "ul",
          items: [
            "join the same VPN, such as Radmin VPN, Tailscale or ZeroTier, and use the host's VPN address (the easy way), or",
            "forward these ports on the host's router and use its public IP: TCP **31313**, **31315** and **20443**, and UDP **31314**.",
          ],
        },
        {
          type: "p",
          text: "All co-op traffic goes through UDP 31314. The games never talk to each other directly.",
        },
        {
          type: "p",
          text: "Never set up a VPN before? [Play over Radmin VPN](/docs/radmin) walks through it with pictures.",
        },
      ],
    },
    {
      id: "graphics",
      title: "Graphics and frame rate",
      blocks: [
        {
          type: "p",
          text: "**Settings** holds the port's own options. **General** has the ones most players change: output resolution, upscaler and preset, frame generation, languages and window mode. The tabs above it go further: graphics, display and FPS, controls, mods and the log. Co-op runs at a fixed 60 FPS: the game sends its updates once per frame, and every player has to send at the same pace.",
        },
        {
          type: "image",
          src: "/docs/launcher-settings-general.webp",
          width: 1200,
          height: 780,
          alt: "Settings, General: Output 1920 by 1080, Frame rate 60 FPS, Upscaler FSR 4, Preset Quality, Game and launcher language, Windowed, Frame generation, and Saves and shortcuts.",
          caption: "Settings, General.",
        },
      ],
    },
    {
      id: "problems",
      title: "When something goes wrong",
      icon: "skull",
      blocks: [
        {
          type: "terms",
          items: [
            [
              "✗ co-op needs 1.09",
              "The launcher found the game without the update. Put the update folder next to the game, or install both from the `.pkg` files.",
            ],
            [
              "Co-op could not start",
              "Usually the folder or the host's IP is empty or wrong. On the host, check that nothing else uses the ports above.",
            ],
            [
              "Nobody can join",
              "The others can't reach the host. Check that everyone is on the same VPN, or that the ports are forwarded, and that the host allowed the firewall prompt. For Radmin VPN, see [its checklist](/docs/radmin#problems).",
            ],
            [
              "A crash",
              `Report it on [GitHub issues](${ISSUES_URL}) with the crash report and say which release every player was on.`,
            ],
          ],
        },
      ],
    },
  ],
};

const S = (name: string) => `/docs/${name}.webp`;

const launcher: DocPage = {
  slug: "launcher",
  title: "Setup and the launcher",
  description:
    "Every screen of Yharnam Together's setup and launcher: installing, the first start, finding or installing the game, the Co-op page, the settings tabs and the status bar.",
  sections: [
    {
      id: "setup",
      title: "The setup",
      icon: "lantern",
      blocks: [
        {
          type: "p",
          text: "The download is one setup `.exe`. It installs for your Windows user only, so it needs no administrator rights, and it holds everything: the port, the launcher, the co-op server and its tools. Nothing from the game is in it.",
        },
        {
          type: "ol",
          items: [
            "Pick the setup's language.",
            "**Next** on the welcome page.",
            "Keep the suggested folder or **Browse…** to another. It needs about 1.6 GB.",
            "Leave **Create a desktop shortcut** ticked if you want one, then **Install**.",
            "**Finish** with **Launch** ticked opens the launcher.",
          ],
        },
        {
          type: "gallery",
          items: [
            {
              label: "Language",
              src: S("setup-language"),
              width: 618,
              height: 286,
              alt: "Select Setup Language: English chosen, with OK and Cancel.",
              caption: "The setup's own language.",
            },
            {
              label: "Welcome",
              src: S("setup-welcome"),
              width: 989,
              height: 738,
              alt: "Welcome to the Setup Wizard, beside the hunter emblem over a night city, with Next and Cancel.",
              caption: "The welcome page.",
            },
            {
              label: "Folder",
              src: S("setup-folder"),
              width: 989,
              height: 738,
              alt: "Select Destination Location with a folder path, Browse, and At least 1.60 GB of free disk space is required.",
              caption: "Where it goes. The default is in your own user folder.",
            },
            {
              label: "Ready",
              src: S("setup-ready"),
              width: 989,
              height: 738,
              alt: "Ready to Install: the destination folder and the task Create a desktop shortcut, with Back, Install and Cancel.",
              caption: "A summary before it starts.",
            },
            {
              label: "Installing",
              src: S("setup-installing"),
              width: 989,
              height: 738,
              alt: "Installing, with Extracting files and a progress bar.",
              caption: "Copying the files.",
            },
            {
              label: "Finished",
              src: S("setup-finished"),
              width: 989,
              height: 738,
              alt: "Completing the Setup Wizard with Launch ticked and Finish.",
              caption: "Done. Finish opens the launcher.",
            },
          ],
        },
        {
          type: "p",
          text: "To remove it, uninstall it from Windows' **Installed apps**. It asks once before it removes anything.",
        },
        {
          type: "image",
          src: S("setup-uninstall"),
          width: 660,
          height: 231,
          alt: "The uninstaller asking: Are you sure you want to completely remove it and all of its components? Yes or No.",
          caption: "The uninstaller's one question.",
        },
      ],
    },
    {
      id: "first-start",
      title: "The first start",
      blocks: [
        {
          type: "p",
          text: "The first time, the launcher opens the **Co-op** page over Home by itself, and the status bar reads **Choose the game folder first**. **PLAY** stays grey until the page is done.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "Co-op opens",
              src: S("launcher-coop-empty"),
              width: 1200,
              height: 693,
              alt: "The Co-op page on first start: the folder empty with the warning Choose your Bloodborne folder, the host's IP and Enemies empty, and Still needed: the game folder, the host's IP, the enemies.",
              caption: "What is still needed is listed beside Done.",
            },
            {
              label: "Game found",
              src: S("launcher-coop-found"),
              width: 1200,
              height: 693,
              alt: "The Co-op page after Find it for me: the folder filled in and a green check, version 01.00 with the 1.09 update next to it. Still needed: the host's IP, the enemies.",
              caption: "Find it for me found the game and its 1.09 update.",
            },
            {
              label: "Done",
              src: S("launcher-coop"),
              width: 1200,
              height: 693,
              alt: "The Co-op page filled in: the folder, the host's IP 10.0.0.2 and Enemies Off (the normal game), with Done lit.",
              caption: "Everything set: Done lights up.",
            },
            {
              label: "Status bar",
              src: S("launcher-bar-first"),
              width: 1200,
              height: 97,
              alt: "The status bar on first start: grey status icons, Start server, Stop server, a grey PLAY and the line Choose the game folder first.",
              caption: "Before the game folder is set.",
            },
          ],
        },
        {
          type: "terms",
          items: [
            [
              "Browse…",
              "Pick the game folder yourself: the folder with `eboot.bin`, at 1.09 or with the 1.09 update next to it.",
            ],
            ["Find it for me", "Looks in the usual places, then on every drive."],
            [
              "Install from PKG…",
              "Installs the game, the update and the DLC from your PS4 `.pkg` files (below).",
            ],
            [
              "Host's IP address",
              "The host's VPN or public address. The host types their own, which is how the launcher knows to start the server on PLAY.",
            ],
            [
              "Enemies",
              "Off is the normal game; Harder gives enemies three times the HP and poise and half again the damage. Everyone picks the same.",
            ],
            [
              "Bring saves from another folder…",
              "Copies your characters from a PS4 save folder, a shadPS4 `user` folder or an older install.",
            ],
          ],
        },
        {
          type: "p",
          text: "The Co-op page stays in the sidebar for later changes. More on each field, and on how the others reach the host, is in [Install and set up](/docs/install#coop-card).",
        },
      ],
    },
    {
      id: "pkg",
      title: "Install from PKG",
      blocks: [
        {
          type: "p",
          text: "If you have the PS4 `.pkg` files instead of a game folder, **Install from PKG…** unpacks them. **Choose PKG files…** takes the game, the 1.09 update and the DLC; **Scan a folder…** finds them for you and ticks the best ones. **Install into** is where the game goes.",
        },
        {
          type: "image",
          src: S("launcher-pkg"),
          width: 853,
          height: 431,
          alt: "The Install from PKG window: Choose PKG files, Scan a folder, the Install into folder with Browse, and Cancel and Install.",
          caption: "Install from PKG.",
        },
      ],
    },
    {
      id: "home",
      title: "Home",
      blocks: [
        {
          type: "p",
          text: "Home shows the project's news: what the latest release changed, with a link to all releases. Below it, **First time here**, **Make a team** and **In the game** open short guides. The launcher updates itself when a new release is out.",
        },
        {
          type: "image",
          src: S("launcher-home"),
          width: 1200,
          height: 780,
          alt: "The launcher's Home page: the sidebar with Home, Co-op and Settings under the hunter emblem, the patch notes Team worlds, cleaner saves, trading by name, the guide tabs First time here, Make a team and In the game, and the status bar with PLAY.",
          caption: "Home, with the latest patch notes.",
        },
      ],
    },
    {
      id: "settings",
      title: "Settings",
      blocks: [
        {
          type: "p",
          text: "**Settings** holds the port's options in tabs. Most players only need **General**. Many of the same options are also in the game's **Insert** menu.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "General",
              src: S("launcher-settings-general"),
              width: 1200,
              height: 780,
              alt: "Settings, General: Output resolution, Frame rate, Upscaler, Preset, Game language, Launcher language, Window mode, Frame generation, and Saves and shortcuts.",
              caption: "General: the options most players change.",
            },
            {
              label: "Graphics",
              src: S("launcher-settings-graphics"),
              width: 1200,
              height: 780,
              alt: "Settings, Graphics: Upscaler, Quality preset, Output resolution, Live resolution changes, Sharpening and Object motion vectors.",
              caption: "Graphics: the upscaler in detail.",
            },
            {
              label: "Display & FPS",
              src: S("launcher-settings-display"),
              width: 1200,
              height: 780,
              alt: "Settings, Display and FPS: Frame rate, Frame cap, Frames ahead of the GPU and Window mode.",
              caption: "Display and FPS. Co-op always runs at 60.",
            },
            {
              label: "Controls",
              src: S("launcher-settings-controls"),
              width: 1200,
              height: 780,
              alt: "Settings, Controls: the controller, the button icons and the mouse camera.",
              caption: "Controls: pad, button icons and mouse.",
            },
            {
              label: "Game",
              src: S("launcher-settings-game"),
              width: 1200,
              height: 780,
              alt: "Settings, Game and effects: the game folder, Install from PKG, the DLC, the saves folder, the game language and the player name.",
              caption: "Game and effects: folders, the DLC and languages.",
            },
            {
              label: "Cheats",
              src: S("launcher-settings-cheats"),
              width: 1200,
              height: 780,
              alt: "Settings, Cheats: cheats and gameplay tweaks, all off.",
              caption: "Cheats and gameplay tweaks, all off by default.",
            },
            {
              label: "Mods",
              src: S("launcher-settings-mods"),
              width: 1200,
              height: 780,
              alt: "Settings, Mods and patches: Load mods, the mods folder and the third-party patches folder.",
              caption:
                "Mods and patches. The game's files are never changed; mods are layered over them at start.",
            },
            {
              label: "Advanced",
              src: S("launcher-settings-advanced"),
              width: 1200,
              height: 780,
              alt: "Settings, Advanced: launcher language, close the launcher when the game starts, animations, shortcuts, clear shader cache and performance options.",
              caption: "Advanced: the launcher itself and diagnostics.",
            },
            {
              label: "Log",
              src: S("launcher-settings-log"),
              width: 1200,
              height: 780,
              alt: "Settings, Log: an empty log with Clear and Copy.",
              caption: "The launcher's log, with Copy for bug reports.",
            },
          ],
        },
      ],
    },
    {
      id: "status-bar",
      title: "The status bar",
      blocks: [
        {
          type: "p",
          text: "The bar at the bottom is on every page: four status icons, the server buttons and **PLAY**. Under the icons, your frame rate, upscaler, resolution and mode. Hover an icon to see what it checks.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "Ready",
              src: S("launcher-bar"),
              width: 1200,
              height: 97,
              alt: "The status bar: four status icons, Start server, Stop server and PLAY, with 60 FPS, FSR 4, 1920 by 1080 and Co-op under the icons.",
              caption: "Ready to play.",
            },
            {
              label: "Server running",
              src: S("launcher-server-running"),
              width: 1200,
              height: 97,
              alt: "The same bar on the host, with the word Running beside the server buttons.",
              caption: "On the host, with the co-op server running.",
            },
            {
              label: "Game",
              src: S("launcher-tip-game"),
              width: 840,
              height: 215,
              alt: "The game icon's tooltip: Game version 01.09: every patch available.",
              caption: "The game: found, at 1.09.",
            },
            {
              label: "Saves",
              src: S("launcher-tip-saves"),
              width: 840,
              height: 215,
              alt: "The saves icon's tooltip: No saves yet: the game creates them in the install's user folder.",
              caption: "The saves: where they are, or that there are none yet.",
            },
            {
              label: "Upscaler",
              src: S("launcher-tip-fsr4"),
              width: 840,
              height: 215,
              alt: "The upscaler icon's tooltip: FSR 4 assets missing (Graphics); FSR 3.1 is used meanwhile.",
              caption: "The upscaler: what is missing, and what is used meanwhile.",
            },
          ],
        },
        {
          type: "terms",
          items: [
            [
              "The four icons",
              "The game, your saves, the graphics card and the upscaler. A green check means ready, a grey dot means not set yet.",
            ],
            [
              "Start server, Stop server",
              "Run the host's server without the game, for example when the host joins later. The word beside them says whether it is running.",
            ],
            [
              "PLAY",
              "Starts the game, and on the host the server first. Grey until the Co-op page is done.",
            ],
          ],
        },
      ],
    },
  ],
};

const play: DocPage = {
  slug: "play",
  title: "Playing together",
  description:
    "How to make a team in the game or join one in progress, travelling apart, chalice dungeons, the map and compass, what the party list shows, how going down and reviving work, and how to give items to a teammate.",
  sections: [
    {
      id: "team",
      title: "Make a team",
      icon: "bell",
      blocks: [
        {
          type: "p",
          text: "**Continue**, **Load Game** or **New Game** on the title screen opens the team screen, The Hunt Together. In the game, press **Insert** (or **L3 + R3**) and choose **Co-op Team**.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "No team",
              src: S("team-none"),
              width: 1440,
              height: 810,
              alt: "The Hunt Together with no team: four empty places, Player 2 and Player 3 in Hunters online, and Create a team, Ready (grey) and Play alone.",
              caption: "Not in a team yet. The hunters online are listed with where they are.",
            },
            {
              label: "Create",
              src: S("team-created"),
              width: 1440,
              height: 810,
              alt: "Player 1's team, 1 of 4: Player 1 as leader, three empty places, and Invite beside Player 2 and Player 3.",
              caption: "Create a team: you lead, and your world is the team's.",
            },
            {
              label: "Invite",
              src: S("team-invite-sent"),
              width: 1440,
              height: 810,
              alt: "The leader's team screen after inviting Player 2, whose Invite button is now grey.",
              caption: "Invite calls a hunter to your team.",
            },
            {
              label: "Invitation",
              src: S("team-invite"),
              width: 1440,
              height: 810,
              alt: "Player 2's team screen with the invitation Player 1 calls you to hunt, and Join and Decline.",
              caption: "The invitation arrives: Join or Decline.",
            },
            {
              label: "Joined",
              src: S("team-joined"),
              width: 1440,
              height: 810,
              alt: "Player 1's team, 2 of 4, seen by Player 2: Player 1 the leader and Player 2 choosing, with a Ready button.",
              caption: "In the team. Pick your character, then Ready.",
            },
            {
              label: "Ready",
              src: S("team-ready"),
              width: 1440,
              height: 810,
              alt: "The same screen with Player 2 marked Ready and the button now reading Not ready.",
              caption: "Ready. The leader begins once everyone is.",
            },
            {
              label: "Begin",
              src: S("team-begin"),
              width: 1440,
              height: 810,
              alt: "The leader's view: Player 2 Ready with a Remove button, and Begin the hunt lit.",
              caption: "The leader's Begin the hunt.",
            },
          ],
        },
        {
          type: "terms",
          items: [
            [
              "Create a team",
              "Makes you the leader. The leader's game hosts the hunt; the server keeps the world.",
            ],
            [
              "Hunters online",
              "Everyone signed in to your server. **Invite** calls one of them to your team.",
            ],
            ["Invitations", "Who called you. **Join** or **Decline**."],
            ["Ready", "You have picked your character. The hunt waits for every hunter."],
            ["Begin the hunt", "Leader only, once everyone is ready."],
            ["Play alone", "Your character plays its own world, without the team."],
            ["Leave the team", "Back to no team. The leader can also **Remove** a member."],
          ],
        },
        {
          type: "p",
          text: "After **Begin the hunt** every game loads its own save. The loading screen, The Hunt Begins, lists each hunter as Loading, Joining the hunt or Ready, and opens for everyone at once. You arrive where your character stood. Starting the game again after a crash puts you back in the team.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "Two hunters",
              src: "/docs/loading.webp",
              width: 1440,
              height: 810,
              alt: "The Hunt Begins loading screen, with cards for Player 1 and Player 2, both Joining the hunt at about 80 percent.",
              caption: "The Hunt Begins: everyone loads, then the hunt opens at once.",
            },
            {
              label: "Three hunters",
              src: S("loading-3"),
              width: 1440,
              height: 810,
              alt: "The Hunt Begins with three cards, Player 1, Player 2 and Player 3, all Joining the hunt at about 90 percent.",
              caption: "A team of three, loading together.",
            },
            {
              label: "Waiting",
              src: S("loading-waiting"),
              width: 1440,
              height: 810,
              alt: "The Hunt Begins with Player 1 and Player 2 Waiting at 0 percent and Player 3 Joining the hunt at 70 percent.",
              caption: "Each card says where that hunter is: Waiting, Joining the hunt or Ready.",
            },
          ],
        },
      ],
    },
    {
      id: "hot-join",
      title: "Join a hunt in progress",
      icon: "blades",
      blocks: [
        {
          type: "p",
          text: "A team can take a new hunter at any time, wherever it is. In the game, press **Insert** (or **L3 + R3**) and choose **Co-op Team**: the same team screen opens. The leader invites, you **Join**, and your game loads into the team's world. If your character last stood on another map than the host, you arrive at the host's place; on the same map, you keep your own spot.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "Co-op Team",
              src: S("hotjoin-page"),
              width: 1440,
              height: 810,
              alt: "Co-op Team opened from the Insert menu, not in a team, with Player 2 and Player 1 In the hunt.",
              caption: "Co-op Team from the Insert menu, during play.",
            },
            {
              label: "Invitation",
              src: S("hotjoin-invite"),
              width: 1440,
              height: 810,
              alt: "The same page with the invitation Player 1 calls you to hunt, and Join and Decline.",
              caption: "A running team calls you.",
            },
            {
              label: "Joining",
              src: S("hotjoin-loading"),
              width: 1440,
              height: 810,
              alt: "The Hunt Begins with Player 1 and Player 2 Ready at 100 percent and Player 3 Joining the hunt at 82 percent.",
              caption: "Only the new hunter loads; the others are already in.",
            },
            {
              label: "Joined",
              src: S("hotjoin-joined"),
              width: 1440,
              height: 810,
              alt: "Player 1's team, 3 of 4: Player 1, Player 2 and Player 3 all In the hunt, with Leave the team.",
              caption: "A team of three, in the hunt.",
            },
            {
              label: "Leader",
              src: S("hotjoin-leader"),
              width: 1440,
              height: 810,
              alt: "The leader's Co-op Team page during the hunt, with Remove under Player 2 and Player 3.",
              caption: "The leader can remove a hunter at any time.",
            },
          ],
        },
      ],
    },
    {
      id: "world",
      title: "One world",
      icon: "lantern",
      blocks: [
        {
          type: "ul",
          items: [
            "Enemies, their health and their deaths are shared. Everyone's hits land on the same health bar.",
            "Doors, levers, shortcuts, lamps and beaten bosses belong to the team's world, which the server keeps on the host's disk.",
            "Items you pick up, Blood Echoes and Insight are your own. NPC gifts are per player, so everyone gets their own.",
            "Everyone travels on their own, so the team can be in different areas at the same time. Nobody is pulled to the host or sent home, and the party list and the map show where everyone is.",
            "Anyone can light a lamp for everyone.",
          ],
        },
      ],
    },
    {
      id: "travel",
      title: "Travel apart",
      icon: "lantern",
      blocks: [
        {
          type: "p",
          text: "You stay in the team wherever you go. One hunter can walk Central Yharnam while another rests in the Hunter's Dream: each plays their own map, the party list keeps both, and the **Map** says where the other one is.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "Player 1",
              src: S("travel-player1"),
              width: 1440,
              height: 810,
              alt: "Player 1 in a clinic hall in Central Yharnam, with Player 2 in the party list on the left.",
              caption: "Player 1 in Central Yharnam.",
            },
            {
              label: "Player 2",
              src: S("travel-player2"),
              width: 1440,
              height: 810,
              alt: "Player 2 on the path of the Hunter's Dream with the Doll nearby, and Player 1 in the party list.",
              caption: "At the same moment, Player 2 in the Hunter's Dream.",
            },
            {
              label: "Player 1's map",
              src: S("travel-map-player1"),
              width: 1440,
              height: 810,
              alt: "Player 1's map of Central Yharnam; under Hunters it lists Player 2 in Hunter's Dream.",
              caption: "Player 1's map lists Player 2 in the Hunter's Dream.",
            },
            {
              label: "Player 2's map",
              src: S("travel-map-player2"),
              width: 1440,
              height: 810,
              alt: "Player 2's map of the Hunter's Dream; under Hunters it lists Player 1 in Central Yharnam.",
              caption: "And Player 2's map lists Player 1 in Central Yharnam.",
            },
          ],
        },
      ],
    },
    {
      id: "chalice",
      title: "Chalice dungeons together",
      icon: "bell",
      blocks: [
        {
          type: "p",
          text: "Chalice dungeons are team altars. When one hunter performs a **Chalice Ritual**, the chalice rests on every teammate's altar too, so each of you can enter the same dungeon from your own **Ritual Altar** in the Hunter's Dream. A teammate who does not own that chalice is given it, and keeps it.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "Ritual",
              src: S("chalice-ritual"),
              width: 1440,
              height: 810,
              alt: "The Chalice Ritual menu with the Pthumeru Chalice chosen, its depth, area and the ritual materials.",
              caption: "One hunter performs the ritual.",
            },
            {
              label: "Ritual done",
              src: S("chalice-ritual-done"),
              width: 1440,
              height: 810,
              alt: "The message: The ritual is finished, and a chalice rests upon the altar. You may now explore the dungeon.",
              caption: "The chalice rests upon the altar.",
            },
            {
              label: "Teammate's altar",
              src: S("chalice-teammate-altar"),
              width: 1440,
              height: 810,
              alt: "Another hunter in the Hunter's Dream at the glowing First Ritual Altar.",
              caption: "A teammate walks up to their own altar.",
            },
            {
              label: "Layers",
              src: S("chalice-layers"),
              width: 1440,
              height: 810,
              alt: "The teammate's altar menu with Pthumerian Labyrinth and its layer Chamber of the Seal.",
              caption: "The team's dungeon is already on it.",
            },
            {
              label: "Inside",
              src: S("chalice-dungeon"),
              width: 1440,
              height: 810,
              alt: "Inside the Pthumerian Labyrinth: two hunters stand before a misty archway.",
              caption: "Both hunters in the same dungeon.",
            },
            {
              label: "Arrival",
              src: S("chalice-arrival"),
              width: 1440,
              height: 810,
              alt: "A hunter arrives in the labyrinth with a teammate just ahead at the archway.",
              caption: "Arriving from the altar, beside the team.",
            },
          ],
        },
      ],
    },
    {
      id: "chalice-team",
      title: "A chalice dungeon with the whole team",
      icon: "skull",
      blocks: [
        {
          type: "p",
          text: "The whole team can gather at the altar while one hunter performs the ritual. Nobody is taken along: when that hunter picks a layer, they alone step into the labyrinth, and the rest stay in the Hunter's Dream until they are ready.",
        },
        {
          type: "ol",
          items: [
            "One hunter performs the **Chalice Ritual**; the others can watch.",
            "Each hunter picks the layer at their own **Ritual Altar**, one after another, in any order.",
            "Everyone arrives at the dungeon's entrance and plays it as a team: enemies, kills and downs are shared as in Yharnam.",
            "Each of you goes back to the **Hunter's Dream** whenever you like; the others play on.",
          ],
        },
        {
          type: "gallery",
          items: [
            {
              label: "Watching",
              src: S("chalice3-watch"),
              width: 1440,
              height: 810,
              alt: "In the Hunter's Dream, Player 3 and Player 2 stand by while Player 1 stands at the First Ritual Altar.",
              caption: "The team looks on while the host performs the ritual.",
            },
            {
              label: "Ritual done",
              src: S("chalice3-ritual-done"),
              width: 1440,
              height: 810,
              alt: "The message: The ritual is finished, and a chalice rests upon the altar. You may now explore the dungeon. Two teammates stand at either side.",
              caption: "The chalice rests on every teammate's altar.",
            },
            {
              label: "First in",
              src: S("chalice3-first-in"),
              width: 1440,
              height: 810,
              alt: "The host alone at the entrance of the Pthumerian Labyrinth, with Player 2 and Player 3 still listed on the left.",
              caption: "The host goes in first; nobody else is pulled along.",
            },
            {
              label: "Own altar",
              src: S("chalice3-own-altar"),
              width: 1440,
              height: 810,
              alt: "Player 2 walks up to the First Ritual Altar in the Dream while Player 3 waits beside it.",
              caption: "The others follow from their own altar when they are ready.",
            },
            {
              label: "Together",
              src: S("chalice3-team"),
              width: 1440,
              height: 810,
              alt: "Player 1, Player 2 and Player 3 side by side in the torchlit hall of a Pthumerian Labyrinth.",
              caption: "Three hunters in the same labyrinth.",
            },
            {
              label: "Back",
              src: S("chalice3-back"),
              width: 1440,
              height: 810,
              alt: "Back in the Hunter's Dream, the three hunters stand on the path below the Doll.",
              caption: "Back in the Dream, one by one, in any order.",
            },
          ],
        },
      ],
    },
    {
      id: "map",
      title: "Map and compass",
      blocks: [
        {
          type: "p",
          text: "The **Map** in the **Insert** menu shows every area of the game with your team on it. The lamps you have lit are listed on the right, and the **Hunters** list says which area each teammate is in. Pick a lit lamp and press **Travel** to go straight there.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "Your area",
              src: S("map-team"),
              width: 1440,
              height: 810,
              alt: "The map of Central Yharnam with the lamps 1st Floor Sickroom, Central Yharnam and Tomb of Oedon, and Player 2 listed under Hunters in Central Yharnam.",
              caption: "Your area, its lamps and the hunters.",
            },
            {
              label: "Three hunters",
              src: S("map-hunters"),
              width: 1440,
              height: 810,
              alt: "The map with Player 2 and Player 1 both listed in Central Yharnam under Hunters, and their markers on the map.",
              caption: "Every teammate is marked on the map.",
            },
            {
              label: "Whole map",
              src: S("map-world"),
              width: 1440,
              height: 810,
              alt: "The map zoomed out over the whole of Central Yharnam and the Workshop.",
              caption: "Triangle shows the whole map; the sticks or a drag move it.",
            },
          ],
        },
        {
          type: "terms",
          items: [
            ["L1 / R1", "The previous or next area."],
            ["L2 / R2, mouse wheel", "Zoom."],
            ["Square", "Back to where you are."],
            ["Triangle", "The whole map."],
            ["R3", "Show or hide the roofs."],
            ["Cross", "Travel to the chosen lamp."],
          ],
        },
        {
          type: "p",
          text: "The compass at the top of the screen shows your teammates while you play: a mark with their initial and how far away they are. When someone is behind you, the mark waits at the edge with an arrow pointing the way.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "Ahead",
              src: S("compass-ahead"),
              width: 1440,
              height: 810,
              alt: "In a dark hall, Player 2 stands ahead; the compass at the top has their mark in the middle.",
              caption: "A teammate straight ahead: the mark sits in the middle of the compass.",
            },
            {
              label: "To the side",
              src: S("compass-hall-1"),
              width: 1440,
              height: 810,
              alt: "Player 2 stands to the right in a doorway; the compass mark has moved right.",
              caption: "Off to the side, the mark slides along the strip.",
            },
            {
              label: "Other side",
              src: S("compass-hall-2"),
              width: 1440,
              height: 810,
              alt: "Player 2 stands to the left; the compass mark is on the left of the strip.",
              caption: "On your left, the mark is on the left.",
            },
            {
              label: "Outside",
              src: S("compass-outside"),
              width: 1440,
              height: 810,
              alt: "Outside a lit church at night, Player 2 is ahead on the left with the compass mark above.",
              caption: "Outdoors it works the same, with the distance under the mark.",
            },
          ],
        },
      ],
    },
    {
      id: "party",
      title: "The party list",
      icon: "tricorn",
      blocks: [
        {
          type: "p",
          text: "Your teammates are listed on the left, under your own bars, in the game's own HUD style: a round portrait, the name, how many **Blood Vials** and **Quicksilver Bullets** they carry, and their health bar. It reads **Down** with the revive progress, or **Dead**. To see which area a teammate is in, open the **Map**.",
        },
        {
          type: "image",
          src: S("party-alive"),
          alt: "In the clinic: the party list on the left shows Player 2 and Player 3, each with a round portrait, 6 Blood Vials, 0 bullets and a health bar.",
          caption: "Two teammates in the party list, with their vials and bullets.",
          width: 1440,
          height: 810,
        },
      ],
    },
    {
      id: "down",
      title: "Going down",
      icon: "hand",
      blocks: [
        {
          type: "p",
          text: "While a teammate stands on your map, a killing blow puts you down instead of killing you. The screen turns red and **YOU ARE DOWN** rises in gold letters. A ring in the middle fills as your teammates strike you back up.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "You are down",
              src: S("you-are-down"),
              width: 1440,
              height: 810,
              alt: "The downed player's screen: red, with YOU ARE DOWN in large gold letters, an empty revive ring and Hold to give up.",
              caption: "The moment you go down.",
            },
            {
              label: "Being revived",
              src: S("you-are-down-revive"),
              width: 1440,
              height: 810,
              alt: "The same red screen with the revive ring at 53 percent while Player 3 stands over you.",
              caption: "A teammate strikes you; the ring shows how close you are.",
            },
            {
              label: "Seen by the others",
              src: S("party-downed"),
              width: 1440,
              height: 810,
              alt: "Player 1's view: Player 2 in the party list with a red ring around the portrait while Player 3 stands nearby.",
              caption: "The others see a red ring on your portrait.",
            },
          ],
        },
        {
          type: "terms",
          items: [
            [
              "Reviving",
              "Strike the downed hunter. The prompt shows **strike to revive** and how far along it is. The first down needs as much damage as their full health, the second twice that, then four times. They get up with half their health and two seconds of safety.",
            ],
            ["Giving up", "Hold the dodge button for three seconds to die at once."],
            [
              "A wipe",
              "When everyone on a map is down, the party dies and wakes at the host's lamp. The map resets as it would after a death, but the team's kills stay, and your Blood Echoes wait where you fell.",
            ],
          ],
        },
        {
          type: "gallery",
          items: [
            {
              label: "At the lamp",
              src: S("wipe-lamp"),
              width: 1440,
              height: 810,
              alt: "Player 1 and Player 2 standing together at the lamp in the clinic after a party wipe.",
              caption: "After a wipe: the whole party wakes at the lamp.",
            },
            {
              label: "Your echoes",
              src: S("wipe-bloodstain"),
              width: 1440,
              height: 810,
              alt: "Back at the lamp with the prompt Acquire Blood Echoes over a bloodstain.",
              caption: "Your echoes stay where you fell until you pick them up.",
            },
          ],
        },
      ],
    },
    {
      id: "give",
      title: "Giving items",
      icon: "vial",
      blocks: [
        {
          type: "p",
          text: "In the inventory, an item's menu lists **Give to** for each teammate nearby, and **Give Blood Echoes to** for echoes. Pick how many; the teammate accepts in the game's own dialog. Weapons go one at a time.",
        },
        {
          type: "gallery",
          items: [
            {
              label: "Inventory",
              src: S("give-inventory"),
              width: 1440,
              height: 810,
              alt: "The inventory with Blood Vial selected: 10 held.",
              caption: "Pick an item in the inventory.",
            },
            {
              label: "Give to",
              src: S("give-menu"),
              width: 1440,
              height: 810,
              alt: "The item's menu: Use, Discard and Give to Player 2.",
              caption: "Give to lists each teammate nearby.",
            },
            {
              label: "How many",
              src: S("give-quantity"),
              width: 1440,
              height: 810,
              alt: "Select quantity of Blood Vials to give to Player 2: 2, Yes or No.",
              caption: "Choose how many.",
            },
            {
              label: "The offer",
              src: S("give-offer"),
              width: 1440,
              height: 810,
              alt: "On Player 2's screen: Player 1 offers you 2 Blood Vials. Accept? Yes or No.",
              caption: "The teammate accepts, or not.",
            },
            {
              label: "Received",
              src: S("give-received"),
              width: 1440,
              height: 810,
              alt: "Player 2 receives Blood Vial times 2 in the game's own item box, with Player 1 standing beside.",
              caption: "They arrive in the game's own item box.",
            },
          ],
        },
      ],
    },
    {
      id: "settings",
      title: "Settings in the game",
      blocks: [
        {
          type: "p",
          text: "**Insert** on the keyboard, or **L3 + R3** on a pad, opens the port's settings over the game, with Co-op Team, Map, Apply and restart the game and Quit to desktop. The frame rate shows 60 (co-op) and can't be changed while playing together.",
        },
        {
          type: "image",
          src: "/docs/insert-menu.webp",
          alt: "The Insert settings menu over the game, on the Display tab, with Co-op Team selected above Map and Quit to desktop.",
          caption: "The Insert menu, with Co-op Team near the bottom.",
          width: 1440,
          height: 810,
        },
        {
          type: "gallery",
          items: [
            {
              label: "Display",
              src: S("insert-display"),
              width: 1200,
              height: 820,
              alt: "Insert settings, Display: output resolution, live resolution changes, window mode, HDR and the FPS counter.",
              caption: "Display.",
            },
            {
              label: "Upscaler",
              src: S("insert-upscaler"),
              width: 1200,
              height: 820,
              alt: "Insert settings, Upscaler: FSR 3.1, quality preset, sharpening, subpixel jitter, motion vectors and frame generation.",
              caption: "Upscaler, changed while you play.",
            },
            {
              label: "Game effects",
              src: S("insert-effects"),
              width: 1200,
              height: 820,
              alt: "Insert settings, Game effects: model detail, chromatic aberration, depth of field, motion blur, ambient occlusion and more.",
              caption: "Game effects: the community patches, each on or off.",
            },
            {
              label: "Controls",
              src: S("insert-controls"),
              width: 1200,
              height: 820,
              alt: "Insert settings, Controls: button icons, mouse camera and sensitivity, and the keyboard and pad bindings.",
              caption: "Controls, with the key for every button.",
            },
            {
              label: "Advanced",
              src: S("insert-advanced"),
              width: 1200,
              height: 820,
              alt: "Insert settings, Advanced: reactive mask options and debug view, then Co-op Team, Map and Quit to desktop.",
              caption: "Advanced, with Co-op Team and Map at the bottom.",
            },
          ],
        },
      ],
    },
  ],
};

const architecture: DocPage = {
  slug: "architecture",
  title: "How it works",
  description:
    "The parts of Yharnam Together: the native PC port, the co-op code built into it, the patched map events and the server one player runs.",
  sections: [
    {
      id: "overview",
      title: "Overview",
      blocks: [
        {
          type: "figure",
          figure: "architecture",
          caption: "Every game keeps one link to the co-op server on the host's PC.",
          alt: "Up to four games (Bloodborne PC with the co-op hooks) each connect over UDP to one co-op server, which holds accounts, presence and the team lobby, the session with one enemy owner per map, the world log saved to disk, and the relay of game packets. The games never connect to each other.",
        },
        {
          type: "p",
          text: "Two programs: the game, which everyone runs, and the server, which the host runs next to their game.",
        },
      ],
    },
    {
      id: "port",
      title: "The port",
      blocks: [
        {
          type: "p",
          text: "Bloodborne PC loads the PS4 game's own x86-64 code and runs it on a small runtime, with shadPS4's renderer drawing through Vulkan. There is no emulator in between, so hooks, memory, threads, files and networking are direct. It keeps what the port offers (high frame rates, 4K, DLSS, FSR, XeSS, frame generation, HDR, ReShade, mouse and keyboard) and builds the co-op code in.",
        },
      ],
    },
    {
      id: "coop-code",
      title: "The co-op code",
      blocks: [
        {
          type: "p",
          text: "The co-op code changes the 1.09 game at about 125 known places, each listed in one table (`emulator/src/core/bloodborne_patch_sites.h`) with the bytes it expects. The launcher warns when another game patch writes over one of them.",
        },
        {
          type: "p",
          text: "It opens the game's session from the team instead of a summoning bell, removes the things that would end a normal visit (boss kills, lamp trips, deaths, map changes, area limits), shares enemies and world state, and adds party death, giving items and the harder enemies option.",
        },
      ],
    },
    {
      id: "events",
      title: "Map events",
      blocks: [
        {
          type: "p",
          text: "Some of the game's event scripts stop early for a summoned player: boss rewards, boss fog, the first visit to the Hunter's Dream. Setup patches copies of your own event files (`tools/re/party_boss_events.py` checks that only the intended instructions changed). The release carries only their expected hashes, never game files.",
        },
      ],
    },
    {
      id: "server",
      title: "The server",
      blocks: [
        {
          type: "p",
          text: "`server/` is a shadNet server with the co-op server added (`server/coop`: the protobuf messages, the world log, the team lobby, trading). shadNet also answers the game's own online requests, so the game goes online without PlayStation Network or any public server. How the server and the games talk is on [the server netcode](/docs/netcode) page.",
        },
      ],
    },
    {
      id: "layout",
      title: "Repository",
      blocks: [
        {
          type: "terms",
          items: [
            [
              "bbport/",
              "Bloodborne PC, the native port, with the launcher and the Windows packaging",
            ],
            [
              "emulator/",
              "shadPS4 with networking; `src/core/bloodborne_*` is the co-op code the port builds in",
            ],
            ["server/", "shadNet and the co-op server"],
            ["backend/", "a stand-in for the game's online services"],
            [
              "tools/",
              "build scripts, local checks, releases, the test rig, reverse-engineering helpers",
            ],
            ["docs/", "design notes"],
            ["website/", "this site"],
          ],
        },
        {
          type: "p",
          text: `Each upstream project is imported with its history, so \`git log\` shows its authors. The design notes go deeper: [BBPORT.md](${blob("docs/BBPORT.md")}), [WORLD_SYNC.md](${blob("docs/WORLD_SYNC.md")}) and [FLAGS.md](${blob("docs/FLAGS.md")}).`,
        },
      ],
    },
  ],
};

const netcode: DocPage = {
  slug: "netcode",
  title: "The server netcode",
  description:
    "Why Yharnam Together replaced peer-to-peer with a server that owns the team's world: the game's native netcode, the UDP protocol, the reliability layer, team worlds and the native team lobby.",
  sections: [
    {
      id: "why",
      title: "Why a server",
      blocks: [
        {
          type: "p",
          text: "Bloodborne's own netcode assumes one host's world, a short visit and peer-to-peer links. Seamless co-op breaks every one of those assumptions, and patching around them took about 120 patch sites and a set of side channels. The rewrite replaces it: **every game keeps one UDP connection to a server, and nothing goes game to game.**",
        },
        {
          type: "figure",
          figure: "topology",
          caption:
            "Four hunters: six peer-to-peer links and a host who owns the world, or four links to a server that owns it.",
          alt: "Left, peer to peer: four hunters each linked to every other, six links. Right, one server in the middle linked to each of the four hunters, four links.",
        },
        {
          type: "figure",
          figure: "links",
          caption:
            "Links to keep alive as the team grows: every pair of players with peer to peer, one per player with a server.",
          alt: "Bar chart. Two players: 1 link peer to peer, 2 with a server. Three players: 3 and 3. Four players: 6 and 4.",
        },
        {
          type: "table",
          head: ["Question", "Decision", "Why"],
          rows: [
            [
              "Peer to peer or server?",
              "Server",
              "Players reach each other over a VPN anyway, so P2P's one advantage (NAT traversal) is gone. The world survives the host leaving, one place orders every change, and each game has one link instead of a mesh. The cost is one extra hop between guests: milliseconds on a VPN.",
            ],
            [
              "Who owns the world?",
              "The server owns its state; the games still play it",
              "The server records and orders kills, flags, doors, lamps, shortcuts, items, enemy HP and map state, keeps them across sessions and crashes, and hands them to joiners. It runs no game.",
            ],
            [
              "Rewrite movement sync?",
              "No",
              "The game's own player and enemy sync keeps running; its packets just travel through the server.",
            ],
            [
              "Wire format?",
              "UDP; protobuf for our messages, raw bytes for the game's",
              "A schema ends hand-written frames breaking older readers. Game packets are many per second and the server never reads them.",
            ],
          ],
        },
      ],
    },
    {
      id: "native",
      title: "What the game does on its own",
      blocks: [
        {
          type: "p",
          text: "Reverse engineering of the 1.09 eboot, checked with a packet trace on the test rig:",
        },
        {
          type: "ul",
          items: [
            "Every game packet goes through one send and one receive point. Packets travel on 68 numbered channels (about 60 in use), some unreliable (movement) and some reliable (everything that must arrive). Hooking those two points lets the co-op code see, drop, rewrite or inject any packet.",
            "The native model is one host's world. The host runs every enemy and sends their positions and HP; a guest's hit on an enemy goes to the host, which applies it. A joining guest gets the host's flags for the host's current map only, once.",
            "Players sync themselves. Each game sends its own player: position, heading, HP and the animation state. A hit on a player goes to the victim's game, which decides its HP.",
          ],
        },
        {
          type: "p",
          text: "When members are on different maps, those assumptions produce double damage, late health bars and joins that only cover one map. That is where the old design's bugs came from.",
        },
      ],
    },
    {
      id: "architecture",
      title: "Architecture",
      blocks: [
        {
          type: "figure",
          figure: "architecture",
          caption: "The server in the middle: what it keeps and what it relays.",
          alt: "Up to four games each connect over one UDP link to the co-op server. The server holds accounts, presence and the team lobby; the session with members, places and one enemy owner per map; the world log, ordered and saved to disk, with snapshots for joiners; and the relay of game packets to the right members.",
        },
        {
          type: "table",
          head: ["Traffic", "Server's role"],
          rows: [
            ["Team and session: lobby, members, who runs which map's enemies", "owns"],
            [
              "World state: kills, flags, doors and levers, lamps, shortcuts, items, enemy HP, map state",
              "owns: orders, stores, settles conflicts, builds snapshots for joiners",
            ],
            ["Player movement, animation and HP", "relays, unread, to members on the same map"],
            [
              "Hits, throws, bullets, forced animations, character data",
              "relays; drops what is meant for members on other maps",
            ],
            ["Enemy movement and state from the map's owner", "relays to the others on that map"],
            ["Enemy damage and HP", "replaced by world-log HP entries, ordered by the server"],
            [
              "Join snapshots",
              "built from the world log, for any map, not only the host's current one",
            ],
          ],
        },
        {
          type: "p",
          text: "Enemies' AI runs in one game per map, chosen by the server: the first member there, handed on when they leave, mid-fight if needed. Each player's character runs in its own game.",
        },
      ],
    },
    {
      id: "protocol",
      title: "The protocol",
      blocks: [
        {
          type: "figure",
          figure: "packet-path",
          caption: "One game packet, from the sending game to the receiving one.",
          alt: "Five steps: the game's send function sees the packet; the channel policy passes it, keeps it to the same map, takes it as ours or drops it; our UDP link adds sequence, acks and resends; the server relays it to the members who need it without parsing it; it is injected into the receiving game's per-member queue.",
        },
        {
          type: "ul",
          items: [
            "One UDP socket per game to the server, on a fixed port next to the TCP one. Each datagram holds messages; each message has a one-byte kind and a length.",
            "Kinds: `Game` (a small fixed header `{channel, reliable bit, sender member, map, visit}` followed by the game's packet untouched), `Proto` (one protobuf message), `Ack` and `Ping`.",
            "Our own reliability layer, per member and per class: sequence numbers, acks, resends and in-order delivery for reliable traffic (the game's reliable channels, world-log entries, snapshots). Unreliable traffic such as movement is never resent; the newest wins. Who is connected, outages and timeouts come from this layer, not from the game's own timers.",
            "Protobuf messages (`server/proto/coop.proto`, shared by the game and the server): `Hello` (account, build, protocol), the lobby (`TeamCreate`, `Invite`, `InviteAnswer`, `TeamState`, `Ready`, `Start`), the session (`Place`, `MemberJoined`, `MemberLeft`, `EnemyOwner`), the world log (`Propose`, `Entries`, `SnapshotRequest`, `Snapshot`), vitals, party death and portraits.",
            "One build for everyone. `Hello` carries the build, and a different build is refused at the lobby with a clear message. There are no compatibility fallbacks.",
          ],
        },
      ],
    },
    {
      id: "traffic",
      title: "What a game sends",
      blocks: [
        {
          type: "p",
          text: "Measured on the test rig: two players at 30 FPS on one PC, from the server's per-kind counters and the games' own logs. The game sends each packet once per teammate, and the server forwards it only to teammates on the same map, so a player on another map gets almost nothing from the others.",
        },
        {
          type: "figure",
          figure: "rates",
          caption:
            "Game packets a game hands to each teammate per second, by its role. The follower's total is measured; its split is partly inferred.",
          alt: "Grouped bar chart; the same numbers are in the table below.",
        },
        {
          type: "table",
          head: ["Kind", ...roles.map((r) => r.name)],
          rows: [
            ...packetRates.map((r) => [
              r.kind,
              String(r.owner),
              String(r.follower),
              String(r.lone),
            ]),
            ["**Total**", ...roles.map((r) => `**~${rateTotal(r.key)}**`)],
          ],
        },
        {
          type: "ul",
          items: [
            "Event sync is most of the owner's packets. It is reliable, so every packet is acknowledged. It ran at about 2 a second in an earlier trace, so the current rate is being looked into.",
            "Frame rate sets most of the rates. Enemy HP goes every 4th frame and player state every 5th, so at the 60 FPS used online both double.",
            "Bandwidth is not the limit. Under 10 KB/s per game with two players; with three, the host's upload peaks around 13 KB/s. The rig link had 2 ms round trips and no resends.",
          ],
        },
        {
          type: "figure",
          figure: "upload",
          caption:
            "Bytes each game sent to the server per second, from the server's 10-second link counters. While the guest was on another map it ran that map's enemies, so its upload rose; the server forwarded none of the host's same-map packets to it.",
          alt: "Line chart over 15 minutes: the host uploads about 6 to 9 KB/s, the guest about 2 to 4 KB/s. While the guest was on another map, from minute 11 to 13.5, the guest's upload rose to about 4.5 KB/s and the host's fell slightly.",
        },
        { type: "h3", text: "Where the server sends each kind" },
        {
          type: "table",
          head: ["Kind", "Route"],
          rows: [
            [
              "Player state, event sync, enemy AI records, forced animations, script calls",
              "teammates on the same map",
            ],
            [
              "Enemy positions, HP and catch-up for joiners",
              "from the map's enemy owner to the others on that map",
            ],
            ["A teammate's hits on enemies, enemy near lists", "to the map's enemy owner only"],
            ["Player hits, throws, bullets, objects", "teammates on the same map"],
            ["Join handshake, character data", "every teammate"],
            ["Map items, door corrections, the boss-room kick", "never forwarded"],
          ],
        },
        { type: "h3", text: "Our own messages" },
        {
          type: "p",
          text: "Besides the game's packets, each game and the server exchange protobuf messages on their own reliable stream: about five a second each way when nothing changes.",
        },
        {
          type: "table",
          head: ["Message", "Direction", "How often"],
          rows: [
            ["Ping", "game and server", "once a second"],
            [
              "Map and enemy owners board",
              "game to server to team",
              "once a second, and at once on a map change",
            ],
            [
              "Vitals (HP, downed)",
              "game to server to team",
              "once a second; 4 a second while someone is down",
            ],
            ["HP proposals", "game to server", "twice a second, and at once on a change"],
            ["Enemy hits", "game to server to team", "one per hit that takes HP"],
            ["World-log entries", "server to team", "heartbeat once a second, at once on news"],
            [
              "Flags (doors, lamps, levers)",
              "game to server",
              "at once, then every half second for 10 seconds",
            ],
            ["Map state report", "host to server", "about every 6 seconds, ~1.25 KB"],
            ["Portrait", "game to server to team", "every 15 seconds, 16 KB"],
          ],
        },
      ],
    },
    {
      id: "team-worlds",
      title: "Team worlds",
      blocks: [
        {
          type: "figure",
          figure: "save-split",
          caption: "The save splits in two: the character stays yours, the world is the team's.",
          alt: "Your save on your PC holds the character: level and stats, gear and inventory, Insight and echoes, appearance, NPC gifts received. The team world on the server holds bosses beaten, lamps, doors, levers, shortcuts, items picked up, NPC and quest states, kills and map state.",
        },
        {
          type: "ul",
          items: [
            "The server stores the worlds itself, with atomic writes and backups. Every accepted world-log entry is written at once, so a crash, a quit or a host change loses nothing, and any member can be the game-level host.",
            "A new team world starts from the leader's progress.",
            "Every save keeps its personal world. Loading into a team world puts the save's own world aside and applies the team's; playing solo again brings it back. The character is never touched by this.",
            "Bosses beaten solo stay in that save's personal world; in a team only the team world counts.",
            "Up to four hunters per team: the game's remote player slots.",
          ],
        },
      ],
    },
    {
      id: "lobby",
      title: "The team lobby",
      blocks: [
        {
          type: "p",
          text: "Continue, Load Game or New Game opens the team screen, where the server keeps the team: its members, the hunters online and the invitations. When the leader begins the hunt, every member's game loads its own save; the first to load becomes the game-level host (the server still owns the world) and the others join its session as they finish loading. After a crash or a quit, starting the game again rejoins the team. Using it is on [Playing together](/docs/play).",
        },
        {
          type: "figure",
          figure: "join-flow",
          caption: "From the title screen to one world, with no bell and no signs.",
          alt: "Six steps: Continue, Load Game or New Game; the team screen, to create a team or accept an invite; everyone picks a character and marks Ready; the leader begins the hunt; loading holds until the team is together; one world, each hunter where they stand.",
        },
      ],
    },

    {
      id: "status",
      title: "Status",
      blocks: [
        {
          type: "p",
          text: `The rewrite was built in phases on a branch, each with tests first and runs on the multi-game rig: a packet trace, a channel policy table, one UDP link to the server, the world log on the server, enemy owners from the server, the native team lobby, the server owning the session (faster joins, host migration, resuming after a drop), one owner per map with a single HP path, server routing with the old peer-to-peer paths deleted, and crash-safe world saves with a server that restarts without ending sessions. It is in main and ships in every release since v0.3 (1.0.0 included).`,
        },
        {
          type: "p",
          text: `The research and the plan with every phase are in the repository's design notes: [NATIVE_NETCODE.md](${blob("docs/NATIVE_NETCODE.md")}) and [SERVER_COOP_PLAN.md](${blob("docs/SERVER_COOP_PLAN.md")}).`,
        },
      ],
    },
  ],
};

const build: DocPage = {
  slug: "build",
  title: "Build from source",
  description:
    "Build the port, the server and the release of Yharnam Together on Windows, run the local checks and the multi-game test rig.",
  sections: [
    {
      id: "prerequisites",
      title: "Prerequisites",
      blocks: [
        { type: "p", text: "Windows only for now." },
        {
          type: "ul",
          items: [
            "The port: [MSYS2](https://www.msys2.org) with the CLANG64 packages listed in `bbport/packaging/windows/README.md`, and a python.org Python 3.10 or later (it freezes the launcher).",
            "The server and emulator: VS 2022 Build Tools (C++ workload, Windows SDK), LLVM 20.1.8 (clang-cl), CMake, Ninja, Qt 6.8.3 `msvc2022_64`.",
            "Checks: Python 3.12 with `clang-format==19.1.7` and `reuse`, and [jj](https://jj-vcs.github.io/jj/) for the local CI script.",
          ],
        },
      ],
    },
    {
      id: "clone",
      title: "Get the code",
      blocks: [
        {
          type: "code",
          lang: "powershell",
          text: `git clone ${REPO_URL}.git C:\\bb\\bloodborne-server
cd C:\\bb\\bloodborne-server
git config submodule.emulator/externals/mesa-kosmickrisp.update none   # macOS-only
git submodule update --init --recursive`,
        },
        {
          type: "p",
          text: "`BB_ROOT` (default `C:\\bb`) holds the checkout. Build output goes to `C:\\bb\\out`, outside the tree.",
        },
      ],
    },
    {
      id: "scripts",
      title: "Scripts",
      blocks: [
        {
          type: "table",
          head: ["Script", "What it does"],
          rows: [
            [
              "`tools/build-bbport.ps1 [-Test]`",
              "builds the port and, with `-Test`, its co-op tests",
            ],
            ["`tools/build-server.ps1`", "builds shadNet and its tests"],
            ["`tools/build-emu.ps1`", "builds the emulator and its unit tests"],
            [
              "`tools/ci-check.ps1 -Component <name>`",
              "local CI per component: format, licenses, build, tests",
            ],
            ["`tools/make-release.ps1 -Version x.y.z`", "builds and checks a release"],
            ["`tools/rig/run_scenario.py`", "the automated multi-game test rig"],
          ],
        },
      ],
    },
    {
      id: "release",
      title: "Make a release",
      blocks: [
        {
          type: "code",
          lang: "powershell",
          text: `tools\\make-release.ps1 -Version 0.1.0              # build and package into C:\\bb\\release
tools\\make-release.ps1 -Version 0.1.0 -Publish     # ... and create a draft GitHub release`,
        },
        {
          type: "p",
          text: "It builds and packages the port and shadNet, adds the launcher's setup and texts, and checks the result: everything a player needs is there, and no game file, save, log or setting is. Build from a clean, pushed commit; check the draft on GitHub, then publish it.",
        },
      ],
    },
    {
      id: "rig",
      title: "The test rig",
      blocks: [
        {
          type: "p",
          text: "`tools/rig` starts two to four low-spec games on one PC, headless, drives them through scenarios (join, lamp trips, boss kills, hits on a shared health bar, party wipes, crashes and rejoins) and checks their logs. Changes run through it alongside the unit and contract tests.",
        },
      ],
    },
    {
      id: "website",
      title: "This website",
      blocks: [
        {
          type: "p",
          text: "`website/` is a Next.js app with shadcn/ui components. `npm ci`, then `npm run dev`; `npm run check` runs the lint, the type check, the tests and the production build.",
        },
      ],
    },
  ],
};

const credits: DocPage = {
  slug: "credits",
  title: "Credits and license",
  description:
    "The projects Yharnam Together is built on, its GPL license, and its relationship to Sony Interactive Entertainment and FromSoftware (none).",
  sections: [
    {
      id: "credits",
      title: "Built on",
      blocks: [
        {
          type: "p",
          text: "This project stands on the work of many people. Each part keeps its authors' history and license.",
        },
        {
          type: "table",
          head: ["Project", "What it brings"],
          rows: [
            [
              "[shadPS4](https://github.com/shadps4-emu/shadPS4)",
              "the renderer and shader recompiler the port draws with, and the network and NP libraries online play uses",
            ],
            [
              "[Wozzardman/shadp2p](https://github.com/Wozzardman/shadp2p)",
              "shadPS4 with peer-to-peer networking and Bloodborne online play: where this co-op started",
            ],
            [
              "[Wozzardman/shadnet-p2p](https://github.com/Wozzardman/shadnet-p2p)",
              "the shadNet server, by the shadPS4 team, with P2P support",
            ],
            [
              "[deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)",
              "Bloodborne PC, the native port",
            ],
            [
              "[Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)",
              "the port on Windows, the launcher in 13 languages, DLSS, FSR 3.1 and FSR 4",
            ],
            [
              "[AdrianCsT/bloodborne_pc](https://github.com/0xCydral/bloodborne_pc)",
              "the redesigned launcher, Install from PKG, frame generation, XeSS, ReShade, the mouse camera and many fixes",
            ],
            [
              "[The Hunter's Dream](https://thehuntersdream.com) by droogie",
              "the community server for Bloodborne's online content, whose service layout the backend follows",
            ],
            [
              "Kyo, Lance McDonald, auser1337, illusion, emoose and others",
              "the community game patches for 1.09: frame rate, resolution, effects",
            ],
            [
              "[Elden Ring Seamless Co-op](https://www.nexusmods.com/eldenring/mods/510) by LukeYui, Elden Ring Nightreign",
              "the ideas: seamless co-op and party death (no code)",
            ],
          ],
        },
      ],
    },
    {
      id: "license",
      title: "License",
      blocks: [
        {
          type: "p",
          text: `GNU General Public License v2.0 or later, like shadPS4 and the port it is built on ([LICENSE](${blob("LICENSE")})). Third-party components keep their own licenses, listed in the release's \`CREDITS.txt\` and \`licenses\` folder.`,
        },
        {
          type: "note",
          text: "Bloodborne is a trademark of Sony Interactive Entertainment. This project contains no game code or assets and is not affiliated with, endorsed or sponsored by Sony Interactive Entertainment or FromSoftware.",
        },
      ],
    },
  ],
};

const radmin: DocPage = {
  slug: "radmin",
  title: "Play over Radmin VPN",
  description:
    "Step by step: install Radmin VPN, make or join the team's network, and put the host's Radmin address into the launcher so everyone reaches the host's co-op server.",
  sections: [
    {
      id: "why",
      title: "Why a VPN",
      icon: "lantern",
      blocks: [
        {
          type: "p",
          text: "The host's PC runs the co-op server, so every other player has to reach that PC. Radmin VPN is the easiest way: it is free, needs no account and no router settings, and puts everyone's PC on one private network with an address that starts with `26.`. Each player does this once.",
        },
        {
          type: "figure",
          figure: "radmin-steps",
          caption: "Who does what.",
          alt: "Two columns. The host: install Radmin VPN, create a network with a name and password, send them to the others, type their own 26. address in the launcher. Every other player: install Radmin VPN, join the network with that name and password, copy the host's 26. address, paste it in the launcher.",
        },
        {
          type: "note",
          text: "Tailscale or ZeroTier work the same way: use the host's address on that network. Forwarded ports work too, see [Install and set up](/docs/install#network).",
        },
      ],
    },
    {
      id: "install-radmin",
      title: "1. Install Radmin VPN (everyone)",
      blocks: [
        {
          type: "ol",
          items: [
            "Download it from [radmin-vpn.com](https://www.radmin-vpn.com) and run the setup. Windows asks to install a network adapter: allow it.",
            "Open **Radmin VPN**. If the big power button at the top is off, press it.",
            "At the top you see your PC's name and its Radmin address, such as `26.12.34.56`. That address stays the same on this PC.",
          ],
        },
      ],
    },
    {
      id: "host-network",
      title: "2. The host makes a network",
      blocks: [
        {
          type: "ol",
          items: [
            "**Network** → **Create a new network**.",
            "Give it a name nobody else is likely to use (for example `yharnam-hunters-4821`) and a password of at least 6 characters, then **Create**.",
            "Send the network's name and password to the other players.",
          ],
        },
      ],
    },
    {
      id: "join-network",
      title: "3. The others join it",
      blocks: [
        {
          type: "ol",
          items: [
            "**Network** → **Join an existing network**.",
            "Type the host's network name and password, then **Join**.",
            "The network shows up in the list with everyone in it. A green dot next to a name means that PC is online.",
          ],
        },
      ],
    },
    {
      id: "launcher",
      title: "4. Put the host's address in the launcher",
      blocks: [
        {
          type: "ol",
          items: [
            "In Radmin VPN, right-click the host's PC in the network and pick **Copy IP address**. The host copies their own address from the top of the window.",
            "In the launcher, open **Co-op** and paste it into **Host's IP address**. Everyone, the host included, types the host's `26.` address: that is how the host's launcher knows to start the server.",
            "Press **Done**, then **PLAY**. The first time, Windows asks once to let co-op through its firewall: say yes.",
          ],
        },
        {
          type: "image",
          src: S("radmin-launcher-ip"),
          width: 1200,
          height: 693,
          alt: "The launcher's Co-op page with the host's Radmin address 26.12.34.56 in Host's IP address and Enemies Off.",
          caption: "The host's Radmin address in the Co-op page. Yours will be different.",
        },
        {
          type: "p",
          text: "The host starts first. Then everyone makes or joins the team in the game, see [Playing together](/docs/play).",
        },
      ],
    },
    {
      id: "check",
      title: "Check the connection",
      blocks: [
        {
          type: "p",
          text: "Right-click the host's PC in Radmin VPN and pick **Ping**. A window shows replies with a time in milliseconds: the lower, the better, and anything under about 100 ms plays well. If it says the request timed out, the two PCs can't see each other yet: see below.",
        },
      ],
    },
    {
      id: "problems",
      title: "When it doesn't connect",
      icon: "skull",
      blocks: [
        {
          type: "terms",
          items: [
            [
              "Another VPN is on",
              "NordVPN and other VPN apps can block Radmin: one of our players could not connect until NordVPN was dealt with. Turn the other VPN off while you play. If it has to stay on, allow local network or LAN traffic in its settings, or leave Radmin VPN and the launcher out of it (split tunnelling).",
            ],
            [
              "The firewall prompt was refused",
              "Press **PLAY** again: the launcher asks again until the rules are there. On the host it adds rules for TCP **31313**, **31315** and **20443** and UDP **31314**; on everyone it lets the game's network helper in. They are named `BB coop port`, `BB coop host TCP` and `BB coop host UDP` in Windows Defender Firewall's inbound rules. Other firewalls (antivirus suites) need the same ports allowed.",
            ],
            [
              "The host's name is grey",
              "The host's PC is off, Radmin VPN is off there, or it lost its connection. The host presses the power button in Radmin VPN.",
            ],
            [
              "Ping works but the game doesn't connect",
              "Check that **Host's IP address** is the host's `26.` address on every PC, the host's too, and that the host pressed PLAY or **Start server** first. Everyone must be on the same release.",
            ],
            [
              "High ping or lag",
              "Radmin VPN sends traffic straight between the PCs when it can and through its relay when it can't, which is slower. Restarting Radmin VPN on both PCs sometimes finds the direct route.",
            ],
          ],
        },
      ],
    },
  ],
};

export const docs: DocPage[] = [
  install,
  radmin,
  launcher,
  play,
  architecture,
  netcode,
  build,
  credits,
];

export function getDoc(slug: string): DocPage | undefined {
  return docs.find((d) => d.slug === slug);
}
