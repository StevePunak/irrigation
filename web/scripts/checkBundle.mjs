import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs'
import { join, relative } from 'node:path'

const URL_PATTERN = /((?:https?|wss?):\/\/[a-z0-9.-]+(?::\d+)?|\/\/[a-z0-9-]+(?:\.[a-z0-9-]+)+(?::\d+)?)([^\s"'`()<>]*)/gi

/** Matched against the whole url, so every entry must end in "/" or a lookalike host passes. */
const ALLOWED_PREFIXES = ['http://www.w3.org/', 'https://www.w3.org/', 'https://react.dev/errors/']

const SCANNED_EXTENSIONS = ['.html', '.js', '.mjs', '.css', '.json', '.svg']

/** Returns every remote origin referenced in `text`, deduplicated. */
export function scanForExternalOrigins(text) {
  const found = new Set()
  for (const match of text.matchAll(URL_PATTERN)) {
    const url = match[0].toLowerCase()
    if (ALLOWED_PREFIXES.some((allowed) => url.startsWith(allowed))) {
      continue
    }
    found.add(match[1])
  }
  return [...found]
}

function walk(dir) {
  const entries = []
  for (const name of readdirSync(dir)) {
    const path = join(dir, name)
    if (statSync(path).isDirectory()) {
      entries.push(...walk(path))
    } else {
      entries.push(path)
    }
  }
  return entries
}

/** Returns every reason this directory is unfit to install. Empty means it is fit. */
export function checkBundleDir(dir) {
  if (existsSync(dir) === false) {
    return [`${dir} does not exist — run "npm run build" first`]
  }

  const problems = []
  const indexPath = join(dir, 'index.html')

  if (existsSync(indexPath) === false) {
    problems.push(`${dir} has no index.html — the irrigation-web recipe fails the image build on this`)
    return problems
  }

  for (const path of walk(dir)) {
    if (path.endsWith('.map')) {
      continue
    }
    if (SCANNED_EXTENSIONS.some((extension) => path.endsWith(extension)) === false) {
      continue
    }

    const origins = scanForExternalOrigins(readFileSync(path, 'utf8'))
    for (const origin of origins) {
      problems.push(
        `${relative(dir, path)} references ${origin} — the controller has no internet and the request will hang`,
      )
    }
  }

  return problems
}
