"use client";

import Image from "next/image";
import { useState } from "react";

import { site, youtubeWatchUrl } from "@/lib/site";

/**
 * The trailer in the site's own frame: a poster from the game with a play button, which swaps in
 * YouTube's player (privacy-enhanced mode) and starts the video. Nothing loads from YouTube until
 * then; without JavaScript the button is a plain link to the video.
 */
export function TrailerPlayer({
  id,
  title = `${site.name} trailer`,
  label = "Watch the trailer",
  poster = "/docs/wipe-lamp.webp",
  small = false,
}: {
  id: string;
  title?: string;
  label?: string;
  poster?: string;
  /** A half-width player: smaller button and text, poster sized for half the page. */
  small?: boolean;
}) {
  const [playing, setPlaying] = useState(false);
  return (
    <figure
      className={
        "panel group relative mx-auto aspect-video w-full overflow-hidden" +
        (small ? "" : " max-w-4xl")
      }
    >
      {playing ? (
        <iframe
          className="absolute inset-0 size-full"
          src={`https://www.youtube-nocookie.com/embed/${id}?autoplay=1&rel=0&modestbranding=1`}
          title={title}
          allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
          referrerPolicy="strict-origin-when-cross-origin"
          allowFullScreen
        />
      ) : (
        <a
          href={youtubeWatchUrl(id)}
          onClick={(event) => {
            event.preventDefault();
            setPlaying(true);
          }}
          className="absolute inset-0 block focus-visible:outline-none"
          aria-label={`Play ${title}`}
        >
          <Image
            src={poster}
            alt=""
            fill
            sizes={small ? "(min-width: 768px) 560px, 100vw" : "(min-width: 896px) 896px, 100vw"}
            className="object-cover brightness-[0.72] transition duration-500 group-hover:brightness-[0.85]"
          />
          <span className="absolute inset-0 bg-[radial-gradient(ellipse_at_center,transparent_30%,rgb(0_0_0/0.6))]" />
          <span className="absolute inset-0 flex flex-col items-center justify-center gap-4">
            <span
              className={
                "flex items-center justify-center rounded-full border border-silver/70 bg-black/55 text-title shadow-[0_0_40px_var(--glow)] transition duration-300 group-hover:scale-105 group-hover:border-title group-hover:text-bone group-focus-visible:border-title " +
                (small ? "size-16" : "size-20 sm:size-24")
              }
            >
              <svg
                viewBox="0 0 24 24"
                aria-hidden="true"
                className={small ? "ml-1 size-7" : "ml-1 size-8 sm:size-10"}
              >
                <path d="M7 4.5v15l12-7.5z" fill="currentColor" />
              </svg>
            </span>
            <span
              className={
                "text-bone tracking-wide " + (small ? "text-lg sm:text-xl" : "text-xl sm:text-2xl")
              }
            >
              {label}
            </span>
          </span>
        </a>
      )}
    </figure>
  );
}
