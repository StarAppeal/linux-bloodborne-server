// The docs' diagrams and charts: server-rendered SVG and HTML, no images to download and no
// client JavaScript. Each one has a text alternative in src/content/docs.ts.

import { ArchitectureDiagram } from "@/components/architecture-diagram";
import { cn } from "@/lib/utils";
import { apart, packetRates, roles, upload } from "@/content/traffic";

function Hunter({ x, y, n }: { x: number; y: number; n: number }) {
  return (
    <g>
      <circle cx={x} cy={y} r="18" fill="var(--card)" stroke="var(--silver)" strokeOpacity="0.7" />
      <text
        x={x}
        y={y + 5}
        textAnchor="middle"
        fontSize="13"
        fill="var(--bone)"
        fontFamily="var(--font-garamond)"
      >
        {n}
      </text>
    </g>
  );
}

function Panel({
  title,
  note,
  children,
}: {
  title: string;
  note: string;
  children: React.ReactNode;
}) {
  return (
    <div className="panel p-4">
      <p className="text-center text-lg text-title">{title}</p>
      <svg viewBox="0 0 240 200" className="mx-auto mt-2 w-full max-w-72" aria-hidden>
        {children}
      </svg>
      <p className="text-center text-sm text-muted-foreground">{note}</p>
    </div>
  );
}

/* Peer to peer against one server: the links each design needs for four hunters. */
export function TopologyFigure() {
  const nodes = [
    { x: 50, y: 40 },
    { x: 190, y: 40 },
    { x: 190, y: 160 },
    { x: 50, y: 160 },
  ];
  const mesh: [number, number][] = [
    [0, 1],
    [0, 2],
    [0, 3],
    [1, 2],
    [1, 3],
    [2, 3],
  ];
  const hub = { x: 120, y: 100 };
  return (
    <div className="grid gap-4 sm:grid-cols-2">
      <Panel
        title="Before: peer to peer"
        note="6 links for 4 hunters; the world lives in the host's game"
      >
        {mesh.map(([a, b]) => (
          <line
            key={`${a}${b}`}
            x1={nodes[a].x}
            y1={nodes[a].y}
            x2={nodes[b].x}
            y2={nodes[b].y}
            stroke="var(--muted-foreground)"
            strokeOpacity="0.6"
            strokeDasharray="4 4"
          />
        ))}
        {nodes.map((p, i) => (
          <Hunter key={i} {...p} n={i + 1} />
        ))}
      </Panel>
      <Panel title="After: one server" note="4 links for 4 hunters; the world lives on the server">
        {nodes.map((p, i) => (
          <line
            key={i}
            x1={p.x}
            y1={p.y}
            x2={hub.x}
            y2={hub.y}
            stroke="var(--silver)"
            strokeOpacity="0.8"
          />
        ))}
        <rect
          x={hub.x - 30}
          y={hub.y - 18}
          width="60"
          height="36"
          fill="var(--primary)"
          stroke="var(--silver)"
        />
        <text
          x={hub.x}
          y={hub.y + 4}
          textAnchor="middle"
          fontSize="11"
          fill="var(--bone)"
          fontFamily="var(--font-garamond)"
          letterSpacing="1.5"
        >
          SERVER
        </text>
        {nodes.map((p, i) => (
          <Hunter key={i} {...p} n={i + 1} />
        ))}
      </Panel>
    </div>
  );
}

/* How many links a team keeps alive: every pair of players peer to peer, one per player with a
   server. */
export const linkCounts = [2, 3, 4].map((n) => ({ players: n, p2p: (n * (n - 1)) / 2, server: n }));

const P2P = "#b08a42";
const SERVER = "#c4443f";

export function LinksChart() {
  const max = 6;
  const width = 520;
  const height = 240;
  const left = 36;
  const bottom = 200;
  const top = 20;
  const groupW = (width - left - 10) / linkCounts.length;
  const barW = 34;
  const y = (v: number) => bottom - (v / max) * (bottom - top);
  return (
    <div className="panel p-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <p className="text-lg text-title">Links per team size</p>
        <ul className="flex gap-4 text-sm text-muted-foreground" aria-label="Legend">
          <li className="flex items-center gap-2">
            <span className="h-2.5 w-4 rounded-sm" style={{ background: P2P }} /> Peer to peer
          </li>
          <li className="flex items-center gap-2">
            <span className="h-2.5 w-4 rounded-sm" style={{ background: SERVER }} /> Server
          </li>
        </ul>
      </div>
      <svg
        viewBox={`0 0 ${width} ${height}`}
        className="mx-auto mt-3 w-full max-w-xl"
        role="img"
        aria-label="Links per team size: 2 players, 1 peer to peer and 2 with a server; 3 players, 3 and 3; 4 players, 6 and 4."
      >
        {[0, 2, 4, 6].map((t) => (
          <g key={t}>
            <line x1={left} x2={width - 10} y1={y(t)} y2={y(t)} stroke="var(--border)" />
            <text
              x={left - 10}
              y={y(t) + 4}
              textAnchor="end"
              fontSize="12"
              fill="var(--muted-foreground)"
            >
              {t}
            </text>
          </g>
        ))}
        {linkCounts.map((d, i) => {
          const cx = left + groupW * i + groupW / 2;
          const bar = (v: number, x: number, color: string, label: string) => (
            <g>
              <title>{`${d.players} players, ${label}: ${v} ${v === 1 ? "link" : "links"}`}</title>
              <rect x={x} y={y(v)} width={barW} height={bottom - y(v)} rx="2" fill={color} />
              <text
                x={x + barW / 2}
                y={y(v) - 6}
                textAnchor="middle"
                fontSize="12"
                fill="var(--muted-foreground)"
              >
                {v}
              </text>
            </g>
          );
          return (
            <g key={d.players}>
              {bar(d.p2p, cx - barW - 1, P2P, "peer to peer")}
              {bar(d.server, cx + 1, SERVER, "server")}
              <text x={cx} y={bottom + 22} textAnchor="middle" fontSize="13" fill="var(--bone)">
                {d.players} players
              </text>
            </g>
          );
        })}
      </svg>
    </div>
  );
}

function Legend({ items }: { items: { name: string; color: string }[] }) {
  return (
    <ul
      className="flex flex-wrap gap-x-4 gap-y-1 text-sm text-muted-foreground"
      aria-label="Legend"
    >
      {items.map((i) => (
        <li key={i.name} className="flex items-center gap-2">
          <span className="h-2.5 w-4 rounded-sm" style={{ background: i.color }} /> {i.name}
        </li>
      ))}
    </ul>
  );
}

/* Packets a game sends each teammate per second, by kind and by the game's role. */
export function RatesChart() {
  const width = 640;
  const left = 150;
  const right = 44;
  const top = 26;
  const barH = 8;
  const gap = 2;
  const groupGap = 14;
  const groupH = roles.length * (barH + gap) - gap;
  const height = top + packetRates.length * (groupH + groupGap) + 22;
  const max = 70;
  const x = (v: number) => left + ((width - left - right) * v) / max;
  return (
    <div className="panel p-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <p className="text-lg text-title">Packets per second, to each teammate</p>
        <Legend items={roles} />
      </div>
      <div className="mt-3 overflow-x-auto">
        <svg
          viewBox={`0 0 ${width} ${height}`}
          className="mx-auto w-full min-w-[520px] max-w-2xl"
          role="img"
          aria-label="Packets per second by kind and role; the same numbers are in the table below."
        >
          {[0, 10, 20, 30, 40, 50, 60, 70].map((t) => (
            <g key={t}>
              <line x1={x(t)} x2={x(t)} y1={top - 6} y2={height - 20} stroke="var(--border)" />
              <text
                x={x(t)}
                y={top - 12}
                textAnchor="middle"
                fontSize="11"
                fill="var(--muted-foreground)"
              >
                {t}
              </text>
            </g>
          ))}
          {packetRates.map((row, i) => {
            const y0 = top + i * (groupH + groupGap);
            return (
              <g key={row.kind}>
                <text x={0} y={y0 + groupH / 2 + 5} fontSize="13" fill="var(--bone)">
                  {row.kind}
                </text>
                {roles.map((role, j) => {
                  const v = row[role.key];
                  const y = y0 + j * (barH + gap);
                  const w = Math.max(v > 0 ? 2 : 0, x(v) - left);
                  return (
                    <g key={role.key}>
                      <title>{`${row.kind}, ${role.name.toLowerCase()}: ${v} per second`}</title>
                      <rect
                        x={left}
                        y={y - 1}
                        width={width - left - right}
                        height={barH + 2}
                        fill="transparent"
                      />
                      {w > 0 && (
                        <rect x={left} y={y} width={w} height={barH} rx="2" fill={role.color} />
                      )}
                      {v >= 20 && (
                        <text
                          x={x(v) + 6}
                          y={y + barH}
                          fontSize="11"
                          fill="var(--muted-foreground)"
                        >
                          {v}/s
                        </text>
                      )}
                    </g>
                  );
                })}
              </g>
            );
          })}
        </svg>
      </div>
    </div>
  );
}

/* Upload from each game to the server over the rig run. */
export function UploadChart() {
  const width = 640;
  const height = 260;
  const l = 40;
  const r = 14;
  const t = 14;
  const b = 40;
  const xmax = 15;
  const ymax = 10;
  const x = (v: number) => l + ((width - l - r) * v) / xmax;
  const y = (v: number) => t + (height - t - b) * (1 - v / ymax);
  const series = [
    { name: "Host, enemy owner", color: roles[0].color, i: 1 as const },
    { name: "Guest", color: roles[1].color, i: 2 as const },
  ];
  return (
    <div className="panel p-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <p className="text-lg text-title">Upload to the server, KB/s</p>
        <Legend items={series} />
      </div>
      <div className="mt-3 overflow-x-auto">
        <svg
          viewBox={`0 0 ${width} ${height}`}
          className="mx-auto w-full min-w-[520px] max-w-2xl"
          role="img"
          aria-label="Upload over 15 minutes: the host sends about 6 to 9 KB/s, the guest about 2 to 4 KB/s. While the guest was on another map, from minute 11 to 13.5, the guest's upload rose to about 4.5 KB/s and the host's fell slightly."
        >
          <rect
            x={x(apart[0])}
            y={t}
            width={x(apart[1]) - x(apart[0])}
            height={height - t - b}
            fill="var(--primary)"
            fillOpacity="0.25"
          />
          <text x={x(apart[0]) + 6} y={t + 14} fontSize="11" fill="var(--bone)">
            guest on another map
          </text>
          {[0, 2, 4, 6, 8, 10].map((v) => (
            <g key={v}>
              <line x1={l} x2={width - r} y1={y(v)} y2={y(v)} stroke="var(--border)" />
              <text
                x={l - 8}
                y={y(v) + 4}
                textAnchor="end"
                fontSize="11"
                fill="var(--muted-foreground)"
              >
                {v}
              </text>
            </g>
          ))}
          {[0, 3, 6, 9, 12, 15].map((v) => (
            <text
              key={v}
              x={x(v)}
              y={height - b + 16}
              textAnchor="middle"
              fontSize="11"
              fill="var(--muted-foreground)"
            >
              {v}
            </text>
          ))}
          <text x={l} y={height - 4} fontSize="11" fill="var(--muted-foreground)">
            minutes since the guest joined
          </text>
          {series.map((s) => {
            const last = upload[upload.length - 1];
            return (
              <g key={s.name}>
                <path
                  d={upload
                    .map((p, k) => `${k ? "L" : "M"}${x(p[0]).toFixed(1)},${y(p[s.i]).toFixed(1)}`)
                    .join("")}
                  fill="none"
                  stroke={s.color}
                  strokeWidth="2"
                  strokeLinejoin="round"
                />
                <circle
                  cx={x(last[0])}
                  cy={y(last[s.i])}
                  r="4"
                  fill={s.color}
                  stroke="var(--card)"
                  strokeWidth="2"
                />
              </g>
            );
          })}
          {upload.map((p) => (
            <rect
              key={p[0]}
              x={x(p[0]) - 3}
              y={t}
              width="6"
              height={height - t - b}
              fill="transparent"
            >
              <title>{`Minute ${p[0].toFixed(1)}: host ${p[1]} KB/s, guest ${p[2]} KB/s`}</title>
            </rect>
          ))}
        </svg>
      </div>
    </div>
  );
}

/* The path of one game packet with the server netcode. */
export function PacketPathFigure() {
  const steps = [
    {
      title: "Game sends",
      text: "one send point sees every packet",
    },
    { title: "Channel policy", text: "pass, same map only, ours, or drop" },
    {
      title: "Our UDP link",
      text: "sequence, ack, resend, in order for reliable channels",
    },
    {
      title: "Server relays",
      text: "to the members who need it; it never parses game packets",
    },
    {
      title: "Game receives",
      text: "injected into the receiver's per-member queue",
    },
  ];
  return (
    <ol className="grid gap-3 sm:grid-cols-5" aria-label="The path of a game packet">
      {steps.map((s, i) => (
        <li key={s.title} className={cn("panel px-4 py-3", i === 3 && "bg-[#1d1710]")}>
          <span className="text-sm text-title">{String(i + 1).padStart(2, "0")}</span>
          <p className="mt-1 text-base text-bone">{s.title}</p>
          <p className="mt-1 text-sm leading-snug text-muted-foreground">{s.text}</p>
        </li>
      ))}
    </ol>
  );
}

/* What stays in a player's save and what the team's world on the server holds. */
export function SaveSplitFigure() {
  const columns = [
    {
      title: "Your save, on your PC",
      sub: "the character",
      items: [
        "level and stats",
        "gear and inventory",
        "Insight and echoes",
        "appearance",
        "NPC gifts you received",
      ],
    },
    {
      title: "The team world, on the server",
      sub: "one per team, saved to disk",
      items: [
        "bosses beaten",
        "lamps, doors, levers, shortcuts",
        "items picked up",
        "NPC and quest states",
        "kills and map state",
      ],
      accent: true,
    },
  ];
  return (
    <div className="grid gap-4 sm:grid-cols-2">
      {columns.map((c) => (
        <div key={c.title} className={cn("panel px-5 py-4", c.accent && "bg-[#1d1710]")}>
          <p className="text-lg text-bone">{c.title}</p>
          <p className="text-sm text-title">{c.sub}</p>
          <ul className="mt-3 space-y-1 text-base text-muted-foreground">
            {c.items.map((i) => (
              <li key={i}>{i}</li>
            ))}
          </ul>
        </div>
      ))}
    </div>
  );
}

/* From the title screen to one world. */
export function JoinFlowFigure() {
  const steps = [
    "Continue, Load Game or New Game",
    "Team screen: create a team or accept an invite",
    "Everyone picks a character and marks Ready",
    "The leader begins the hunt",
    "Loading holds until the team is together",
    "One world, each hunter where they stand",
  ];
  return (
    <ol className="relative grid gap-3 border-l border-title/40 pl-6" aria-label="Joining a team">
      {steps.map((s, i) => (
        <li key={s} className="relative">
          <span
            aria-hidden
            className="absolute top-2 -left-[1.95rem] size-2.5 rotate-45 border border-title bg-background"
          />
          <span className="text-sm text-title">{String(i + 1).padStart(2, "0")}</span>{" "}
          <span className="text-base text-foreground">{s}</span>
        </li>
      ))}
    </ol>
  );
}

/* Radmin VPN: what the host does and what every other player does. */
export function RadminStepsFigure() {
  const columns = [
    {
      title: "The host",
      items: [
        "install Radmin VPN",
        "create a network: a name and a password",
        "send both to the others",
        "type their own 26. address in the launcher",
      ],
      accent: true,
    },
    {
      title: "Every other player",
      items: [
        "install Radmin VPN",
        "join the network with that name and password",
        "copy the host's 26. address",
        "paste it in the launcher",
      ],
    },
  ];
  return (
    <div className="grid gap-4 sm:grid-cols-2">
      {columns.map((c) => (
        <div key={c.title} className={cn("panel px-5 py-4", c.accent && "bg-[#1d1710]")}>
          <p className="text-lg text-bone">{c.title}</p>
          <ol className="mt-3 space-y-1 text-base text-muted-foreground">
            {c.items.map((i, n) => (
              <li key={i}>
                <span className="text-sm text-title">{String(n + 1).padStart(2, "0")}</span> {i}
              </li>
            ))}
          </ol>
        </div>
      ))}
    </div>
  );
}

export const figures = {
  architecture: ArchitectureDiagram,
  topology: TopologyFigure,
  links: LinksChart,
  rates: RatesChart,
  upload: UploadChart,
  "packet-path": PacketPathFigure,
  "save-split": SaveSplitFigure,
  "join-flow": JoinFlowFigure,
  "radmin-steps": RadminStepsFigure,
} as const;

export type FigureId = keyof typeof figures;
