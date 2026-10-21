// Common questions, shown on the guide's index and published as FAQPage structured data.

export const faq: { q: string; a: string }[] = [
  {
    q: "Do I need my own copy of Bloodborne?",
    a: "Yes. Nothing from the game is included or downloaded. You need Bloodborne for PS4 at version 1.09, as a game folder with the update or as the PS4 .pkg files, which the launcher can install.",
  },
  {
    q: "How many can play together?",
    a: "Up to four in a team. A running team can take a new hunter at any time.",
  },
  {
    q: "Do I need to rent a server?",
    a: "No. One player hosts the server on their own PC; the launcher starts it with the game. The others reach that PC over a VPN such as Radmin VPN, Tailscale or ZeroTier, or through forwarded ports. The guide walks through Radmin VPN step by step.",
  },
  {
    q: "Is it an emulator?",
    a: "No. It runs on Bloodborne PC, a native port: the PS4 game's own code runs on your CPU and its graphics go through Vulkan.",
  },
  {
    q: "Can players on different versions play together?",
    a: "No. Everyone, the server included, runs the same release, and a different build is turned away.",
  },
  {
    q: "Is it official?",
    a: "No. It is a free, open-source fan project under the GPL, with no game code or assets, and is not affiliated with Sony Interactive Entertainment or FromSoftware.",
  },
];
