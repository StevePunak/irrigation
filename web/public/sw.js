// The no-op fetch handler is what makes Chrome offer "Install app"; deleting it removes the install prompt.
// Nothing is cached: a cached /api/status would show valves closed while they run.
self.addEventListener('install', () => self.skipWaiting())
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()))
self.addEventListener('fetch', () => {})
