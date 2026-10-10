export const SETTING_KEYS = {
  rainDelayUntil: 'rain_delay_until',
  masterEnabled: 'master_enabled',
  maxZoneSeconds: 'max_zone_seconds',
  logLevel: 'log_level',
  maxConcurrentZones: 'max_concurrent_zones',
  panelRunMinutes: 'panel_run_minutes',
  latitude: 'latitude',
  longitude: 'longitude',
} as const

export const DEFAULT_MAX_ZONE_SECONDS = 3600

/** The daemon's default for max_concurrent_zones, used until /api/status reports the real cap. */
export const DEFAULT_MAX_CONCURRENT_ZONES = 2

/** The daemon rejects any max_concurrent_zones outside 1 through this value with a 400. */
export const MAX_CONCURRENT_ZONES_LIMIT = 8

/** The daemon's default for panel_run_minutes, used when the setting is absent. */
export const DEFAULT_PANEL_RUN_MINUTES = 10

/** The daemon rejects any panel_run_minutes outside 1 through this value with a 400. */
export const PANEL_RUN_MINUTES_LIMIT = 60

/** Disabled only by the exact value '0', matching the daemon's scheduler. Widening the match shows watering off while the valves still run. */
export function parseMasterEnabled(value: string | undefined): boolean {
  return value !== '0'
}

/** The daemon rejects every value except '0' and '1' with a 400. */
export function serializeMasterEnabled(enabled: boolean): string {
  return enabled ? '1' : '0'
}

export function parseInteger(value: string | undefined, fallback: number): number {
  const normalised = (value ?? '').trim()
  if (/^-?\d+$/.test(normalised) === false) {
    return fallback
  }
  return Number(normalised)
}

/** An absent key and an empty value are the same thing on the daemon side. */
export function parseInstant(value: string | undefined): string | null {
  const normalised = (value ?? '').trim()
  if (normalised.length === 0 || Number.isFinite(Date.parse(normalised)) === false) {
    return null
  }
  return normalised
}

export const LATITUDE_LIMIT = 90
export const LONGITUDE_LIMIT = 180

/** Mirrors the daemon's check: plain decimal degrees, no exponent or plus sign, within ±limit. */
export function parseCoordinate(text: string, limit: number): string | null {
  const normalised = text.trim()
  if (/^-?\d{1,3}(\.\d+)?$/.test(normalised) === false || Math.abs(Number(normalised)) > limit) {
    return null
  }
  return normalised
}
