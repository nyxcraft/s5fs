# The documentation site

A static site built from the Markdown this repo already carries — `README.md`,
`HANDOFF.md`, and everything under `docs/`. Nothing is duplicated here: editing
a document is the only step, and there is no second copy to keep in step with
the first.

```
gh-pages/
  site.json        the site model: metadata, the document groups, and each page's
                   card copy (a doc in docs/ is published even if unregistered)
  build_site.py    the generator (markdown-it-py); writes public/
  check_links.py   fails if the built site links to something it lacks
  templates/       home.html, doc.html, page.html
  assets/          site.css, logo-s5fs.svg
  public/          build output — gitignored, published by Actions
```

## Build it locally

```sh
pip install markdown-it-py
python3 gh-pages/build_site.py
python3 gh-pages/check_links.py
python3 -m http.server -d gh-pages/public 8000   # then open localhost:8000
```

## How it is published

`.github/workflows/pages.yml` builds the site and publishes it as a Pages
artifact on every push to `main` that touches a document, plus on demand via
**Actions → Build and deploy docs to GitHub Pages → Run workflow**.

**One-time setup:** in the repository's **Settings → Pages**, set **Source** to
**GitHub Actions**. Nothing is ever committed to a `gh-pages` *branch* — the
built site exists only as the Pages artifact, which is why `gh-pages/public/` is
gitignored.

## Adding documentation

Dropping a `.md` file into `docs/` is enough — it is discovered, published, and
listed under the fallback group. The site cannot silently omit a document
somebody wrote.

Registering it in `site.json`'s `docs` array is what gives it good card copy and
puts it in the right place:

```json
{ "slug": "allocator", "source": "docs/allocator.md", "group": "format",
  "tagline": "The chained free list",
  "blurb": "One sentence for the card on the home page.",
  "featured": true }
```

| field | meaning |
|---|---|
| `slug` | the URL segment; two pages resolving to the same slug **fail the build** rather than silently overwriting each other |
| `source` | repo-relative path; a missing file fails the build |
| `group` | which home-page group the card sits in (the ordered `groups` array) |
| `title` / `summary` | override the doc's own H1 and first paragraph |
| `tagline` / `blurb` | the card's subtitle and body |
| `template` | `doc` (left rail of the page's H2s, shown one section at a time) or `page` (one continuous scroll, for long reference documents) |
| `featured` | promote it to a card in the "Start here" grid |

## What the generator does to a document

- **Cross-document `.md` links are rewritten** to the built page's URL, resolved
  by **path**, so the same link works when the file is read on GitHub and when
  it is read here.
- **Links to source files** — a header, a command's implementation, an example
  spec — are rewritten to a GitHub blob URL, because they have no counterpart in
  the site. A link to a file that does not exist is left alone, so it stays
  visible as the mistake it is instead of becoming a plausible dead URL.
- **The first `# H1` is removed** from the body and used as the page title, so a
  document does not show its own title twice.
- **`<!--include: path-->`** is replaced by that file's current contents in a
  fenced block, for showing source without copying it.
