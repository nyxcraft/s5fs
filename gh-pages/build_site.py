#!/usr/bin/env python3
"""Build the s5fs documentation site from the Markdown the repo already carries.

Nothing is duplicated: the sources are README.md, HANDOFF.md and docs/*.md, rendered rather
than copied. Editing a document is the only step. A doc dropped into docs/ is discovered and
published even if nobody remembers to register it in site.json -- registering it only adds the
card copy and decides where it sits on the home page.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import shutil
import unicodedata
from html import escape
from pathlib import Path

try:
    from markdown_it import MarkdownIt
except ImportError as exc:  # pragma: no cover - human setup path
    raise SystemExit(
        "markdown-it-py is required to build the docs site.\n"
        "Install it with: pip install markdown-it-py"
    ) from exc


ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIR = ROOT / "gh-pages"  # build machinery: site.json, templates/, assets/
OUTPUT_DIR = ROOT / "gh-pages" / "public"  # built site (gitignored; published by Actions)
HEADER_LOGO = "assets/logo-s5fs.svg"


def slugify(text: str) -> str:
    normalized = unicodedata.normalize("NFKD", text)
    ascii_text = normalized.encode("ascii", "ignore").decode("ascii")
    slug = re.sub(r"[^a-zA-Z0-9]+", "-", ascii_text.lower()).strip("-")
    return slug or "section"


def read_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def render_template(path: Path, context: dict[str, str]) -> str:
    text = path.read_text(encoding="utf-8")
    for key, value in context.items():
        text = text.replace(f"{{{{ {key} }}}}", value)
    return text


def relative_href(from_file: Path, to_file: Path) -> str:
    return os.path.relpath(to_file, from_file.parent).replace(os.sep, "/")


def doc_title(text: str) -> str | None:
    """The first level-1 heading, which every doc opens with."""
    for line in text.splitlines():
        if line.startswith("# "):
            return line[2:].strip()
    return None


_INLINE_MD = re.compile(r"`([^`]*)`|\*\*([^*]+)\*\*|\*([^*]+)\*|_([^_]+)_|\[([^\]]+)\]\([^)]*\)")


def _strip_inline(text: str) -> str:
    def repl(m: re.Match[str]) -> str:
        return next(g for g in m.groups() if g is not None)

    return _INLINE_MD.sub(repl, text)


def doc_lede(text: str, limit: int = 220) -> str:
    """The doc's first ordinary paragraph, flattened to a one-line summary -- skipping the H1,
    blockquotes (status banners), lists, tables and fenced code. Truncated at a sentence end."""
    lines = text.splitlines()
    para: list[str] = []
    in_fence = False
    for line in lines:
        s = line.strip()
        if s.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        if not s:
            if para:
                break
            continue
        if s.startswith(("#", ">", "-", "*", "|", "<", "    ")):
            if para:
                break
            continue
        para.append(s)
    summary = _strip_inline(" ".join(para)).replace("  ", " ").strip()
    if len(summary) <= limit:
        return summary
    cut = summary[:limit]
    dot = cut.rfind(". ")
    return (cut[: dot + 1] if dot > 60 else cut.rstrip() + "…").strip()


class MarkdownRenderer:
    def __init__(self) -> None:
        self.md = MarkdownIt("commonmark", {"html": True, "typographer": True})
        self.md.enable("table")
        self.md.enable("strikethrough")

    def render(self, text: str) -> dict[str, object]:
        tokens = self.md.parse(text)
        slug_counts: dict[str, int] = {}
        toc: list[dict[str, object]] = []
        title = None
        first_h1_index = None

        for index, token in enumerate(tokens):
            if token.type != "heading_open":
                continue
            level = int(token.tag[1])
            if index + 1 >= len(tokens):
                continue
            inline = tokens[index + 1]
            if inline.type != "inline":
                continue
            heading_text = inline.content.strip()
            if not heading_text:
                continue

            base_slug = slugify(heading_text)
            count = slug_counts.get(base_slug, 0)
            slug_counts[base_slug] = count + 1
            anchor = base_slug if count == 0 else f"{base_slug}-{count + 1}"
            token.attrSet("id", anchor)

            if level == 1 and title is None:
                title = heading_text
                first_h1_index = index
            elif level in (2, 3):
                toc.append({"level": level, "anchor": anchor, "text": heading_text})

        if first_h1_index is not None:
            del tokens[first_h1_index : first_h1_index + 3]

        html = self.md.renderer.render(tokens, self.md.options, {})
        return {"title": title, "toc": toc, "html": html}


def ensure_clean_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def copy_tree(source: Path, destination: Path) -> None:
    if source.exists():
        shutil.copytree(source, destination, dirs_exist_ok=True)


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def rewrite_md_links(
    html: str, source: Path, current_output: Path, output_dir: Path,
    source_to_output: dict[str, str], github_href: str, branch: str,
) -> str:
    """Rewrite `<a href="...md">` links so cross-document links work in the built site too.

    The Markdown sources use ordinary relative `.md` links (e.g. `[design](docs/design.md)`) so
    they also resolve when browsed on GitHub. Here each `.md` target is resolved against the
    SOURCE document's directory -- by PATH, not basename -- into its repo-relative path. A
    target that is a site page becomes that page's pretty URL (relative to the page being
    written, `#anchor` preserved); a `.md` file that exists in the tree but is NOT a site page
    becomes a GitHub blob link, the same citation the non-`.md` links get; anything else is left
    alone."""

    def replace(match: re.Match[str]) -> str:
        href = match.group(1)
        if "://" in href or href.startswith(("#", "mailto:")):
            return match.group(0)
        path, _, anchor = href.partition("#")
        resolved = (source.parent / path).resolve()
        try:
            rel_src = str(resolved.relative_to(ROOT))
        except ValueError:
            return match.group(0)
        suffix = "#" + anchor if anchor else ""
        target = source_to_output.get(rel_src)
        if target is not None:
            rel = relative_href(current_output, output_dir / target)
            return f'href="{escape(rel + suffix)}"'
        if resolved.exists():
            return f'href="{escape(f"{github_href}/blob/{branch}/{rel_src}{suffix}")}"'
        return match.group(0)

    return re.sub(r'href="([^"]+\.md(?:#[^"]*)?)"', replace, html)


def rewrite_repo_links(html: str, source: Path, github_href: str, branch: str) -> str:
    """Point links to repo files that are NOT site pages at the file on GitHub.

    The Markdown deliberately links to source -- a header, a command's implementation, an
    example spec -- because on GitHub those resolve and are the best possible citation. They
    have no counterpart in the built site, so left alone they 404. Resolving each against the
    SOURCE document's directory and, if the file really exists in the tree, rewriting it to a
    blob URL keeps the citation working in both places. A link to something that does not exist
    is left untouched, so it stays visible as the mistake it is rather than becoming a plausible
    dead URL."""

    def replace(match: re.Match[str]) -> str:
        href = match.group(1)
        if "://" in href or href.startswith(("#", "mailto:", "/")):
            return match.group(0)
        path, _, anchor = href.partition("#")
        if not path or path.endswith(".md"):
            return match.group(0)  # .md was handled already
        target = (source.parent / path).resolve()
        try:
            rel = target.relative_to(ROOT)
        except ValueError:
            return match.group(0)  # escapes the repo
        if not target.exists():
            return match.group(0)
        kind = "tree" if target.is_dir() else "blob"
        suffix = "#" + anchor if anchor else ""
        return f'href="{escape(f"{github_href}/{kind}/{branch}/{rel}{suffix}")}"'

    return re.sub(r'href="([^"]+)"', replace, html)


INCLUDE_LANGS = {
    ".py": "python",
    ".sh": "bash",
    ".c": "c",
    ".h": "c",
    ".json": "json",
    ".ini": "ini",
    ".md": "markdown",
    ".js": "javascript",
    ".css": "css",
    ".html": "html",
}


def expand_includes(text: str) -> str:
    """Expand `<!--include: path-->` (path relative to the repo root) into a fenced code block
    of that file's current contents -- so an example spec or a header lives in one place and is
    shown on the page without copy-paste drift. Runs before Markdown rendering."""

    def replace(match: re.Match[str]) -> str:
        rel = match.group(1).strip()
        try:
            body = (ROOT / rel).read_text(encoding="utf-8").rstrip("\n")
        except OSError:
            return match.group(0)  # leave the directive untouched if the file is missing
        lang = INCLUDE_LANGS.get(Path(rel).suffix, "")
        return f"```{lang}\n{body}\n```"

    return re.sub(r"<!--\s*include:\s*([^>]+?)\s*-->", replace, text)


def rewrite_img_src(html: str, current_output: Path, output_dir: Path) -> str:
    """Rewrite content `<img src="...">` (authored relative to docs/) to a path relative to the
    page being written. The `docs/media/` tree is copied to `<site>/media/` at build time."""

    def replace(match: re.Match[str]) -> str:
        src = match.group(1)
        if "://" in src or src.startswith(("/", "data:")):
            return match.group(0)
        norm = src[2:] if src.startswith("./") else src
        return f'src="{escape(relative_href(current_output, output_dir / norm))}"'

    return re.sub(r'src="([^"]+)"', replace, html)


def build_toc(entries: list[dict[str, object]]) -> str:
    if not entries:
        return ""
    items = []
    for entry in entries:
        level = int(entry["level"])
        cls = f"section-nav__link section-nav__link--l{level}"
        items.append(
            f'<li><a class="{cls}" href="#{escape(str(entry["anchor"]))}">'
            f"{escape(str(entry['text']))}</a></li>"
        )
    return '<ul class="section-nav__list">\n' + "\n".join(items) + "\n</ul>"


def collect_pages(config: dict) -> list[dict]:
    """Explicit docs from site.json, then any remaining docs/*.md auto-discovered from the tree.

    Registering a doc is optional -- it decides the card copy and the home-page group. Dropping
    a file into docs/ is enough to publish it, so the site cannot silently omit a document
    somebody wrote."""
    pages: list[dict] = []
    seen_sources: set[str] = set()
    seen_slugs: dict[str, str] = {}

    def add(entry: dict, *, auto: bool) -> None:
        src = entry["source"]
        if src in seen_sources:
            return
        path = ROOT / src
        if not path.exists():
            raise SystemExit(f"site.json lists {src!r}, which does not exist")
        seen_sources.add(src)
        slug = entry["slug"]
        if slug in seen_slugs:
            raise SystemExit(
                f"duplicate page slug {slug!r}: {seen_slugs[slug]} and {src} would write the "
                f"same URL. Give one an explicit, distinct slug in site.json."
            )
        seen_slugs[slug] = src
        text = path.read_text(encoding="utf-8")
        pages.append(
            {
                "slug": slug,
                "title": entry.get("title") or doc_title(text) or slug,
                "summary": entry.get("summary") or doc_lede(text),
                "source": src,
                "template": entry.get("template", "doc"),
                "output": str(Path(slug) / "index.html"),
                "featured": bool(entry.get("featured", False)),
                "group": entry.get("group", ""),
                "tagline": entry.get("tagline", ""),
                "blurb": entry.get("blurb", ""),
                "auto": auto,
            }
        )

    for entry in config.get("docs", []):
        add(entry, auto=False)

    # Anything in docs/ nobody registered still gets published, in the fallback group.
    fallback = config.get("fallback_group", "reference")
    for path in sorted((ROOT / "docs").glob("*.md")):
        rel = str(path.relative_to(ROOT))
        if rel in seen_sources:
            continue
        add({"slug": path.stem, "source": rel, "group": fallback}, auto=True)

    for page in pages:
        page["href"] = page["output"].replace(os.sep, "/")
    return pages


def main() -> int:
    parser = argparse.ArgumentParser(description="Build the s5fs docs site.")
    parser.add_argument("--output", default=str(OUTPUT_DIR), help="Build output directory")
    args = parser.parse_args()

    output_dir = Path(args.output).resolve()
    config = read_json(SOURCE_DIR / "site.json")
    renderer = MarkdownRenderer()

    ensure_clean_dir(output_dir)
    copy_tree(SOURCE_DIR / "assets", output_dir / "assets")
    copy_tree(ROOT / "docs" / "media", output_dir / "media")
    write_text(output_dir / ".nojekyll", "")

    docs_pages = collect_pages(config)
    pages_by_slug = {p["slug"]: p for p in docs_pages}
    groups = config.get("groups", [])

    # Map each source's repo-relative path -> its built page, so cross-document `.md` links get
    # rewritten to pretty URLs by PATH.
    source_to_output = {p["source"]: p["output"] for p in docs_pages}

    github_href = config.get("github_href", "#")

    def href_from(from_output: Path, slug: str) -> str:
        page = pages_by_slug.get(slug)
        return relative_href(from_output, output_dir / (page["output"] if page else "index.html"))

    # A compact, fixed header: the three whole-toolkit entry points. The full document
    # catalog lives on the home page, not the header.
    NAV = [("overview", "Overview"), ("design", "Design"), ("user-guide", "User guide")]

    def top_nav(from_output: Path) -> str:
        out = []
        for slug, label in NAV:
            if slug in pages_by_slug:
                out.append(
                    f'        <a href="{escape(href_from(from_output, slug))}">{escape(label)}</a>'
                )
        return "\n".join(out)

    def doc_groups(from_output: Path) -> str:
        """The home page's document catalog: every page grouped by role, each a card linking to
        the document. Unregistered docs land in the fallback group rather than vanishing."""
        out = []
        for group in groups:
            members = [p for p in docs_pages if p.get("group") == group["id"]]
            if not members:
                continue
            out.append('      <section class="tool-group">')
            out.append(f'        <h3 class="tool-group__title">{escape(group["name"])}</h3>')
            out.append('        <div class="tool-grid">')
            for page in members:
                href = escape(relative_href(from_output, output_dir / page["output"]))
                tagline = page["tagline"] or page["title"]
                blurb = page["blurb"] or page["summary"]
                out.append("\n".join([
                    '          <article class="tool-card">',
                    f'            <h4><a href="{href}">{escape(page["title"])}</a></h4>',
                    f'            <p class="tool-card__tagline">{escape(tagline)}</p>',
                    f"            <p>{escape(blurb)}</p>",
                    '          </article>',
                ]))
            out.append('        </div>')
            out.append('      </section>')
        return "\n".join(out)

    featured_pages = [p for p in docs_pages if p["featured"]] or docs_pages[:4]
    docs_cards = []
    for page in featured_pages:
        docs_cards.append(
            "\n".join([
                '<article class="doc-card">',
                f'  <h3><a href="{escape(page["href"])}">{escape(page["title"])}</a></h3>',
                f"  <p>{escape(page['summary'])}</p>",
                f'  <a class="doc-card__link" href="{escape(page["href"])}">Open</a>',
                "</article>",
            ])
        )

    license_page = pages_by_slug.get("license")
    hero_primary = href_from(output_dir / "index.html", "user-guide")
    hero_secondary = href_from(output_dir / "index.html", "design")
    # Pinned in site.json so the build is reproducible: the committed gh-pages/public/ is
    # diffed against a fresh build in CI, and a year that changed on its own every January
    # would fail that check with nothing having been edited.
    year = str(config.get("copyright_year") or datetime.date.today().year)
    copyright_holder = config.get("copyright", config["site_name"])
    status = config.get("status")
    status_badge = f'<span class="brand__badge">{escape(status)}</span>' if status else ""
    attribution_lines = config.get("attribution") or []
    attribution = (
        '<p class="site-footer__attr">'
        + "<br>".join(escape(line) for line in attribution_lines)
        + "</p>"
        if attribution_lines
        else ""
    )
    attribution_hero = (
        '<div class="hero__attr">'
        + "".join(f"<p>{escape(line)}</p>" for line in attribution_lines)
        + "</div>"
        if attribution_lines
        else ""
    )

    home_html = render_template(
        SOURCE_DIR / "templates" / "home.html",
        {
            "site_name": escape(config["site_name"]),
            "site_tagline": escape(config["site_tagline"]),
            "site_description": escape(config["site_description"]),
            "logo_href": escape(HEADER_LOGO),
            "home_href": escape("index.html"),
            "status_badge": status_badge,
            "github_href": escape(github_href),
            "docs_href": escape(href_from(output_dir / "index.html", "overview")),
            "top_nav": top_nav(output_dir / "index.html"),
            "doc_groups": doc_groups(output_dir / "index.html"),
            "primary_href": escape(hero_primary),
            "secondary_href": escape(hero_secondary),
            "docs_cards": "\n".join(docs_cards),
            "year": year,
            "copyright": escape(copyright_holder),
            "attribution": attribution,
            "attribution_hero": attribution_hero,
            "license_href": escape(
                href_from(output_dir / "index.html", "license") if license_page else github_href
            ),
        },
    )
    write_text(output_dir / "index.html", home_html)

    for page in docs_pages:
        source_path = ROOT / page["source"]
        rendered = renderer.render(expand_includes(source_path.read_text(encoding="utf-8")))
        output_path = output_dir / page["output"]
        content_html = rewrite_md_links(
            str(rendered["html"]), source_path, output_path, output_dir, source_to_output,
            github_href, config.get("branch", "main"),
        )
        content_html = rewrite_repo_links(
            content_html, source_path, github_href, config.get("branch", "main")
        )
        content_html = rewrite_img_src(content_html, output_path, output_dir)
        toc_html = build_toc(rendered["toc"])  # type: ignore[arg-type]
        template = "page.html" if page["template"] == "page" else "doc.html"

        doc_html = render_template(
            SOURCE_DIR / "templates" / template,
            {
                "page_title": escape(page["title"]),
                "site_name": escape(config["site_name"]),
                "site_tagline": escape(config["site_tagline"]),
                "page_summary": escape(page["summary"]),
                "assets_href": escape(
                    relative_href(output_path, output_dir / "assets" / "site.css")
                ),
                "logo_href": escape(relative_href(output_path, output_dir / HEADER_LOGO)),
                "home_href": escape(relative_href(output_path, output_dir / "index.html")),
                "status_badge": status_badge,
                "github_href": escape(github_href),
                "docs_href": escape(href_from(output_path, "overview")),
                "top_nav": top_nav(output_path),
                "toc": toc_html,
                "source_title": escape(str(rendered["title"] or "")),
                "content": content_html,
                "year": year,
                "copyright": escape(copyright_holder),
                "attribution": attribution,
                "license_href": escape(
                    href_from(output_path, "license") if license_page else github_href
                ),
            },
        )
        write_text(output_path, doc_html)

    print(f"Built {len(docs_pages)} pages into {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
