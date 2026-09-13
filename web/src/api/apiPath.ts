/** Mount point nginx proxies to the daemon. The bundle never contains the daemon's own prefix. */
export const API_PREFIX = '/api'

const DAEMON_PREFIX = '/admin'

/** Returns the browser-side url for a daemon route, e.g. `/status` -> `/api/status`. */
export function apiUrl(path: string): string {
  return `${API_PREFIX}${path}`
}

/**
 * Rewrites a browser-side url to the daemon route it proxies to. Used by the dev
 * server proxy; nginx performs the same rewrite in production.
 */
export function daemonPath(url: string): string {
  if (url.startsWith(`${API_PREFIX}/`) === false && url !== API_PREFIX) {
    throw new Error(`daemonPath expects a ${API_PREFIX} url, received "${url}"`)
  }
  return `${DAEMON_PREFIX}${url.slice(API_PREFIX.length)}`
}
