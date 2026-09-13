export const SETTING_KEYS = {
  rainDelayUntil: 'rain_delay_until',
  masterEnabled: 'master_enabled',
  maxZoneSeconds: 'max_zone_seconds',
  logLevel: 'log_level',
} as const

export const DEFAULT_MAX_ZONE_SECONDS = 3600

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
