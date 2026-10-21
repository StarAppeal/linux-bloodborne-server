import { DocArticle, docMetadata } from "@/components/doc-article";

export const metadata = docMetadata("radmin");

export default function Page() {
  return <DocArticle slug="radmin" />;
}
