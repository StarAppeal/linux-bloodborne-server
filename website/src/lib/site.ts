// Everything the site says about itself in one place: names, links and the deployment's URL.

export const REPO = "lKaniel/bloodborne-server";
export const REPO_URL = `https://github.com/${REPO}`;
// The site's own address. seamlesscoop.dev is the project's domain; this game's site is its
// bloodborne. subdomain (the old bloodborne-seamless-coop.vercel.app address redirects).
export const SITE_DOMAIN = "bloodborne.seamlesscoop.dev";

export const RELEASES_URL = `${REPO_URL}/releases`;
export const LATEST_RELEASE_URL = `${RELEASES_URL}/latest`;
export const ISSUES_URL = `${REPO_URL}/issues`;
// The project's own channels, shown in the header and footer.
export const SOCIAL_LINKS = [
  {
    id: "youtube",
    label: "YouTube",
    href: "https://www.youtube.com/channel/UCY_8yufPmucngGywT5ChOBQ",
  },
  { id: "discord", label: "Discord", href: "https://discord.gg/YD2YsN4y8r" },
  { id: "x", label: "X", href: "https://x.com/yharnamtogether" },
] as const;
export const blob = (path: string) => `${REPO_URL}/blob/main/${path}`;

export const site = {
  name: "Yharnam Together: Bloodborne Seamless Co-op",
  shortName: "Yharnam Together",
  tagline: "Play all of Bloodborne with your friends, in one world.",
  description:
    "Free, open-source seamless co-op for Bloodborne on PC. Up to four players share one world on a native port of the PS4 game, connected through a small server one of them runs.",
  keywords: [
    "Yharnam Together",
    "Bloodborne",
    "Bloodborne PC",
    "Bloodborne PC port",
    "Bloodborne seamless co-op",
    "Bloodborne co-op mod",
    "Bloodborne multiplayer mod",
    "Bloodborne online co-op",
    "Bloodborne 4 player co-op",
    "play Bloodborne with friends",
    "Bloodborne chalice dungeons co-op",
    "seamless co-op mod",
    "self-hosted game server",
    "open source",
  ],
  license: "GPL-2.0-or-later",
} as const;

// Where the server netcode stands: it ships in every release since v0.3 (1.0.0 included), which the download
// page offers and the guide describes. The note shows on the install page while it is not shipped.
export const netcodeStatus = {
  shippedInRelease: true,
  label: "Shipped",
  note: "",
} as const;

type Env = Record<string, string | undefined>;

/**
 * The deployment's public origin, without a trailing slash: what canonical links, the sitemap,
 * llms.txt and the social preview's address use. NEXT_PUBLIC_SITE_URL wins; any build on Vercel
 * (previews too, so they never claim to be the real page) uses SITE_DOMAIN, not Vercel's own
 * production URL, which can be the old vercel.app address; a local build uses localhost.
 */
export function resolveSiteUrl(env: Env = process.env): string {
  const explicit = env.NEXT_PUBLIC_SITE_URL?.trim();
  if (explicit) return withProtocol(explicit);
  if (env.VERCEL) return withProtocol(SITE_DOMAIN);
  return "http://localhost:3000";
}

function withProtocol(host: string): string {
  const url = /^https?:\/\//.test(host) ? host : `https://${host}`;
  return url.replace(/\/+$/, "");
}

export const siteUrl = resolveSiteUrl();

export function absoluteUrl(path = "/"): string {
  return new URL(path, `${siteUrl}/`).toString();
}

// The trailer on YouTube; NEXT_PUBLIC_TRAILER_YOUTUBE_ID swaps in another video without a code change.
export const TRAILER_YOUTUBE_ID = "MBlRNpzBqmo";
export const youtubeWatchUrl = (id: string) => `https://www.youtube.com/watch?v=${id}`;

// Longer gameplay videos, shown under the trailer on the landing page (and near the top of the README).
export const GAMEPLAY_VIDEOS = [
  { id: "2p2LF5YfhRM", title: "Gameplay showcase 1", poster: "/docs/chalice3-team.webp" },
  { id: "JzaeJXrHaFE", title: "Gameplay showcase 2", poster: "/docs/compass-outside.webp" },
] as const;

/**
 * The trailer. NEXT_PUBLIC_TRAILER_YOUTUBE_ID may hold a YouTube video id or a full YouTube URL;
 * unset or not a video, the landing page plays TRAILER_YOUTUBE_ID.
 */
export function resolveTrailerId(env: Env = process.env): string {
  const raw = env.NEXT_PUBLIC_TRAILER_YOUTUBE_ID?.trim();
  if (!raw) return TRAILER_YOUTUBE_ID;
  const fromUrl = raw.match(/(?:youtu\.be\/|[?&]v=|\/embed\/|\/shorts\/)([\w-]{11})/);
  const id = fromUrl ? fromUrl[1] : raw;
  return /^[\w-]{11}$/.test(id) ? id : TRAILER_YOUTUBE_ID;
}
