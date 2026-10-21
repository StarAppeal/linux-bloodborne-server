import Image from "next/image";
import Link from "next/link";

import { ArtIcon } from "@/components/art-icon";
import { DownloadButton } from "@/components/download-button";
import { Emblem } from "@/components/logo";
import { Heading } from "@/components/ornament";
import { StartSteps, type StartStep } from "@/components/start-steps";
import { TrailerPlayer } from "@/components/trailer-player";
import { VideoSlot } from "@/components/video-slot";
import type { IconName } from "@/content/docs";
import { JsonLd } from "@/lib/json-ld";
import { GAMEPLAY_VIDEOS, REPO_URL, absoluteUrl, site, siteUrl } from "@/lib/site";

// The rest of what changes: four short ones with an icon, then two with a picture.
const changes: { term: string; icon: IconName; text: string }[] = [
  {
    term: "One world",
    icon: "lantern",
    text: "Enemies, bosses, doors, lamps and shortcuts are shared by the whole team.",
  },
  {
    term: "Your own things",
    icon: "vial",
    text: "Pickups, Blood Echoes and Insight stay yours. NPC gifts come to each of you.",
  },
  {
    term: "Free travel",
    icon: "bell",
    text: "Everyone goes where they like. Nobody is pulled along or sent home.",
  },
  {
    term: "Up to four",
    icon: "tricorn",
    text: "Four hunters in a team, made in the game's own menus.",
  },
];

const showcases = [
  {
    term: "Native port",
    text: "Bloodborne PC, not an emulator: 60 FPS, 4K, DLSS, FSR and XeSS, set in the launcher.",
    href: "/docs/install#graphics",
    src: "/docs/launcher-settings-general.webp",
    width: 1200,
    height: 780,
    alt: "The launcher's settings: resolution, frame rate and upscaler.",
  },
  {
    term: "Your server",
    text: "One of you hosts it next to the game. No accounts, no cloud: everyone types the host's address.",
    href: "/docs/install#coop-card",
    src: "/docs/launcher-coop.webp",
    width: 1200,
    height: 693,
    alt: "The launcher's Co-op page with the game folder and the host's address filled in.",
  },
];

const features: {
  title: string;
  icon: IconName;
  text: string;
  href: string;
  src: string;
  width: number;
  height: number;
  alt: string;
}[] = [
  {
    title: "Your teammates on screen",
    icon: "tricorn",
    text: "Each teammate sits on the left under your own bars: a portrait, their name, their Blood Vials and bullets, and their health. The map shows which area each of them is in.",
    href: "/docs/play#party",
    src: "/docs/party-alive.webp",
    width: 1440,
    height: 810,
    alt: "In the clinic, the party list shows Player 2 and Player 3 with portraits, vials, bullets and health bars.",
  },
  {
    title: "Down, not dead",
    icon: "hand",
    text: "A killing blow puts you down while a teammate is on your map. They strike you to raise you; if everyone goes down, the party wakes at the host's lamp.",
    href: "/docs/play#down",
    src: "/docs/you-are-down.webp",
    width: 1440,
    height: 810,
    alt: "The downed player's red screen with YOU ARE DOWN in gold letters and Hold to give up.",
  },
  {
    title: "Hand things over",
    icon: "vial",
    text: "Give a teammate Blood Vials, weapons or Blood Echoes from the inventory. They accept in the game's own dialog.",
    href: "/docs/play#give",
    src: "/docs/give-offer.webp",
    width: 1440,
    height: 810,
    alt: "On Player 2's screen: Player 1 offers you 2 Blood Vials. Accept?",
  },
  {
    title: "Your team, any time",
    icon: "bell",
    text: "Press Insert, or L3 + R3 on a pad, and choose Co-op Team. It shows your team, who is online and who invited you, without leaving the game.",
    href: "/docs/play#team",
    src: "/docs/insert-team.webp",
    width: 1440,
    height: 810,
    alt: "The Co-op Team page from the Insert menu: Player 1, Player 2 and Player 3 In the hunt, one empty place, Hunters online and Invitations.",
  },
  {
    title: "Ready, then the hunt",
    icon: "bell",
    text: "Members press Ready on the team screen. The leader begins the hunt and everyone loads into the same world.",
    href: "/docs/play#team",
    src: "/docs/team-begin.webp",
    width: 1440,
    height: 810,
    alt: "The leader's team screen: Player 1 and Player 2, who is Ready, and Begin the hunt lit.",
  },
  {
    title: "Join a hunt in progress",
    icon: "blades",
    text: "A running team takes a new hunter any time. Only the newcomer loads, and arrives beside the host if they were elsewhere.",
    href: "/docs/play#hot-join",
    src: "/docs/hotjoin-loading.webp",
    width: 1440,
    height: 810,
    alt: "The Hunt Begins: Player 1 and Player 2 Ready at 100 percent, Player 3 Joining the hunt.",
  },
  {
    title: "Chalice dungeons together",
    icon: "skull",
    text: "One hunter performs the ritual while the team looks on. Each of you then enters from your own altar, in any order, and you meet in the same dungeon.",
    href: "/docs/play#chalice-team",
    src: "/docs/chalice3-team.webp",
    width: 1440,
    height: 810,
    alt: "Player 1, Player 2 and Player 3 side by side in the torchlit hall of a Pthumerian Labyrinth.",
  },
  {
    title: "Find each other",
    icon: "lantern",
    text: "The map marks every teammate and the area they are in, and travels to any lit lamp. A compass on screen points the way and the distance.",
    href: "/docs/play#map",
    src: "/docs/map-hunters.webp",
    width: 1440,
    height: 810,
    alt: "The map of Central Yharnam with lit lamps and Player 1 and Player 2 under Hunters.",
  },
];

const steps: StartStep[] = [
  {
    title: "Install",
    text: "Run the setup on every PC. It installs for your Windows user and opens the launcher.",
    href: "/docs/launcher#setup",
    src: "/docs/setup-welcome.webp",
    width: 989,
    height: 738,
    alt: "The setup's welcome page beside the hunter emblem.",
  },
  {
    title: "Fill in the Co-op page",
    text: "Everyone types the host's address, the host included. PLAY then starts the server on the host's PC.",
    href: "/docs/install#coop-card",
    src: "/docs/launcher-coop.webp",
    width: 1200,
    height: 693,
    alt: "The Co-op page with the game folder and the host's address.",
  },
  {
    title: "Make a team",
    text: "Press PLAY, invite each other on the team screen and begin the hunt.",
    href: "/docs/play#team",
    src: "/docs/team-begin.webp",
    width: 1440,
    height: 810,
    alt: "The team screen with two hunters, before loading in.",
  },
];

export default function Home() {
  return (
    <>
      <section className="mx-auto grid max-w-6xl items-center gap-x-14 gap-y-12 px-4 pt-14 pb-20 sm:px-6 lg:grid-cols-[minmax(0,5fr)_minmax(0,7fr)] lg:pt-20">
        <div>
          <Emblem
            priority
            className="size-40 drop-shadow-[0_0_40px_rgb(120_138_168/0.25)] sm:size-48"
          />
          <h1 className="mt-6 text-5xl leading-[1.05] font-normal text-bone sm:text-6xl">
            Yharnam Together
            <span className="mt-2 block text-3xl text-title sm:text-4xl">
              Bloodborne Seamless Co-op
            </span>
          </h1>
          <p className="mt-6 max-w-md text-xl">
            Play the whole game with your friends in one shared world, from the first lamp to the
            last boss.
          </p>
          <div className="mt-8 flex flex-wrap items-center gap-x-6 gap-y-4">
            <DownloadButton />
            <Link
              href="/docs/install"
              className="text-lg text-title underline decoration-title/40 underline-offset-4 hover:text-bone"
            >
              Install guide
            </Link>
          </div>
          <p className="mt-6 text-base text-muted-foreground">
            Windows 10 and 11. You need your own Bloodborne 1.09 for PS4.
          </p>
        </div>
        <figure>
          <Image
            src="/docs/loading.webp"
            alt="The Hunt Begins loading screen: cards for Player 1 and Player 2, both joining the hunt."
            width={1440}
            height={810}
            priority
            sizes="(min-width: 1024px) 640px, 100vw"
            className="h-auto w-full border-y border-silver/30 shadow-[0_30px_80px_rgb(0_0_0/0.7)]"
          />
          <figcaption className="mt-3 text-base text-muted-foreground italic">
            The Hunt Begins: the team loads into one world.
          </figcaption>
        </figure>
      </section>

      <section aria-label="Trailer" className="mx-auto max-w-6xl px-4 sm:px-6">
        <VideoSlot />
      </section>

      <section aria-labelledby="gameplay" className="mx-auto mt-28 max-w-6xl px-4 sm:px-6">
        <Heading id="gameplay">Gameplay</Heading>
        <div className="mt-10 grid gap-6 md:grid-cols-2">
          {GAMEPLAY_VIDEOS.map((v) => (
            <TrailerPlayer
              key={v.id}
              id={v.id}
              title={v.title}
              label={v.title}
              poster={v.poster}
              small
            />
          ))}
        </div>
      </section>

      <section aria-labelledby="features" className="mx-auto mt-28 max-w-6xl px-4 sm:px-6">
        <Heading id="features">In the game</Heading>
        <div className="mt-12 space-y-20">
          {features.map((f, i) => (
            <article
              key={f.title}
              className="grid items-center gap-x-14 gap-y-6 md:grid-cols-[minmax(0,7fr)_minmax(0,5fr)]"
            >
              <Image
                src={f.src}
                alt={f.alt}
                width={f.width}
                height={f.height}
                sizes="(min-width: 768px) 620px, 100vw"
                className={
                  "h-auto w-full border-y border-silver/30 shadow-[0_20px_60px_rgb(0_0_0/0.6)]" +
                  (i % 2 ? " md:order-2" : "")
                }
              />
              <div>
                <h3 className="flex items-center gap-4 text-2xl text-bone">
                  <ArtIcon name={f.icon} className="size-11" />
                  {f.title}
                </h3>
                <p className="mt-3">{f.text}</p>
                <Link
                  href={f.href}
                  className="mt-4 inline-block text-title underline decoration-title/40 underline-offset-4 hover:text-bone"
                >
                  More in the guide
                </Link>
              </div>
            </article>
          ))}
        </div>

        <dl className="mt-24 grid gap-5 sm:grid-cols-2 lg:grid-cols-4">
          {changes.map((c) => (
            <div key={c.term} className="panel px-6 pt-6 pb-7">
              <ArtIcon name={c.icon} className="size-14" />
              <dt className="mt-4 text-xl text-bone">{c.term}</dt>
              <dd className="mt-1.5 text-muted-foreground">{c.text}</dd>
            </div>
          ))}
        </dl>
        <div className="mt-5 grid gap-5 md:grid-cols-2">
          {showcases.map((c) => (
            <Link
              key={c.term}
              href={c.href}
              className="panel group flex flex-col overflow-hidden transition-colors hover:border-silver/50"
            >
              <div className="relative aspect-[16/9] overflow-hidden border-b border-silver/20">
                <Image
                  src={c.src}
                  alt={c.alt}
                  width={c.width}
                  height={c.height}
                  sizes="(min-width: 768px) 560px, 100vw"
                  className="h-full w-full object-cover object-top transition-transform duration-700 group-hover:scale-[1.03]"
                />
              </div>
              <div className="px-6 pt-5 pb-6">
                <h3 className="text-xl text-bone">{c.term}</h3>
                <p className="mt-1.5 text-muted-foreground">{c.text}</p>
              </div>
            </Link>
          ))}
        </div>
        <p className="mt-10 max-w-2xl text-muted-foreground">
          Instead of the game&apos;s peer-to-peer links, every game connects to one small server
          that keeps the team&apos;s world.{" "}
          <Link href="/docs/netcode" className="text-title hover:text-bone">
            Why a server
          </Link>
        </p>
      </section>

      <section aria-labelledby="start" className="mx-auto mt-28 max-w-6xl px-4 sm:px-6">
        <Heading id="start">Getting started</Heading>
        <div className="mt-10">
          <StartSteps steps={steps} />
        </div>

        <div className="panel mt-20 flex flex-col gap-6 px-6 py-8 sm:px-10 md:flex-row md:items-center md:justify-between">
          <p className="max-w-xl text-lg">
            One installer for every player. Everyone in a team, the host included, uses the same
            release.
          </p>
          <DownloadButton />
        </div>
      </section>

      <JsonLd
        data={{
          "@context": "https://schema.org",
          "@type": "SoftwareApplication",
          name: site.name,
          alternateName: [site.shortName, "Bloodborne Seamless Co-op"],
          description: site.description,
          image: absoluteUrl("/opengraph-image"),
          applicationCategory: "GameApplication",
          applicationSubCategory: "Game mod",
          keywords: site.keywords.join(", "),
          author: { "@type": "Person", name: "lKaniel", url: "https://github.com/lKaniel" },
          operatingSystem: "Windows 10, Windows 11",
          url: siteUrl,
          downloadUrl: `${siteUrl}/download`,
          license: `https://spdx.org/licenses/${site.license}.html`,
          isAccessibleForFree: true,
          offers: { "@type": "Offer", price: "0", priceCurrency: "USD" },
          codeRepository: REPO_URL,
        }}
      />
    </>
  );
}
