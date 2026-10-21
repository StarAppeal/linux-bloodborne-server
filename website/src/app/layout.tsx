import { Analytics } from "@vercel/analytics/next";
import type { Metadata, Viewport } from "next";
import { EB_Garamond } from "next/font/google";

import { SiteFooter } from "@/components/site-footer";
import { Backdrop } from "@/components/backdrop";
import { SiteHeader } from "@/components/site-header";
import { JsonLd } from "@/lib/json-ld";
import { REPO_URL, SOCIAL_LINKS, site, siteUrl } from "@/lib/site";
import "./globals.css";

const garamond = EB_Garamond({
  variable: "--font-garamond",
  subsets: ["latin"],
  weight: ["400", "500"],
  style: ["normal", "italic"],
});

export const metadata: Metadata = {
  metadataBase: new URL(siteUrl),
  title: {
    default: `${site.shortName}: Bloodborne seamless co-op on PC`,
    template: `%s | ${site.shortName}`,
  },
  description: site.description,
  applicationName: site.name,
  keywords: [...site.keywords],
  alternates: { canonical: "/" },
  openGraph: {
    type: "website",
    siteName: site.name,
    title: site.name,
    description: site.description,
    url: "/",
    locale: "en",
  },
  authors: [{ name: "lKaniel", url: REPO_URL }],
  creator: "lKaniel",
  formatDetection: { telephone: false, address: false, email: false },
  twitter: {
    card: "summary_large_image",
    title: site.name,
    description: site.description,
  },
  robots: { index: true, follow: true },
  category: "games",
};

export const viewport: Viewport = {
  themeColor: "#0d0f13",
  colorScheme: "dark",
};

export default function RootLayout({ children }: LayoutProps<"/">) {
  return (
    <html lang="en" className={garamond.variable}>
      <body className="flex min-h-dvh flex-col">
        <Backdrop />
        <a
          href="#main"
          className="sr-only z-50 bg-card px-4 py-2 text-bone focus:not-sr-only focus:fixed focus:top-2 focus:left-2"
        >
          Skip to content
        </a>
        <SiteHeader />
        <main id="main" className="flex-1">
          {children}
        </main>
        <SiteFooter />
        <JsonLd
          data={{
            "@context": "https://schema.org",
            "@type": "WebSite",
            name: site.name,
            alternateName: [site.shortName, "Bloodborne Seamless Co-op"],
            inLanguage: "en",
            url: siteUrl,
            description: site.description,
            sameAs: [REPO_URL, ...SOCIAL_LINKS.map((link) => link.href)],
          }}
        />
        <Analytics />
      </body>
    </html>
  );
}
