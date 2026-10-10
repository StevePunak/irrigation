import { describe, expect, it } from 'vitest'
import {
  DEFAULT_MAX_ZONE_SECONDS,
  LATITUDE_LIMIT,
  LONGITUDE_LIMIT,
  SETTING_KEYS,
  parseCoordinate,
  parseInstant,
  parseInteger,
  parseMasterEnabled,
  serializeMasterEnabled,
} from './settingsMap'

describe('parseMasterEnabled', () => {
  it('reads "0" as disabled', () => {
    expect(parseMasterEnabled('0')).toBe(false)
  })

  it('reads "1" as enabled', () => {
    expect(parseMasterEnabled('1')).toBe(true)
  })

  it('reads an absent or empty value as enabled, as the daemon does', () => {
    expect(parseMasterEnabled(undefined)).toBe(true)
    expect(parseMasterEnabled('')).toBe(true)
  })

  it('reads every other spelling as enabled, as the daemon does', () => {
    expect(parseMasterEnabled('false')).toBe(true)
    expect(parseMasterEnabled('off')).toBe(true)
    expect(parseMasterEnabled(' 0')).toBe(true)
  })

  it('serializes to the only two values the daemon accepts', () => {
    expect(serializeMasterEnabled(false)).toBe('0')
    expect(serializeMasterEnabled(true)).toBe('1')
    expect(parseMasterEnabled(serializeMasterEnabled(false))).toBe(false)
    expect(parseMasterEnabled(serializeMasterEnabled(true))).toBe(true)
  })
})

describe('parseInteger', () => {
  it('reads a decimal string', () => {
    expect(parseInteger('1800', 60)).toBe(1800)
    expect(parseInteger('0', 60)).toBe(0)
  })

  it('falls back for an absent, empty or unparseable value', () => {
    expect(parseInteger(undefined, DEFAULT_MAX_ZONE_SECONDS)).toBe(3600)
    expect(parseInteger('', DEFAULT_MAX_ZONE_SECONDS)).toBe(3600)
    expect(parseInteger('half an hour', DEFAULT_MAX_ZONE_SECONDS)).toBe(3600)
  })

  it('falls back for a partially numeric string', () => {
    expect(parseInteger('1800s', 60)).toBe(60)
    expect(parseInteger('18 00', 60)).toBe(60)
  })
})

describe('parseInstant', () => {
  it('returns the string when it is a usable timestamp', () => {
    expect(parseInstant('2026-09-15T00:00:00Z')).toBe('2026-09-15T00:00:00Z')
  })

  it('treats absent and empty as unset', () => {
    expect(parseInstant(undefined)).toBeNull()
    expect(parseInstant('')).toBeNull()
  })

  it('treats an unparseable timestamp as unset', () => {
    expect(parseInstant('not a date')).toBeNull()
  })
})

describe('SETTING_KEYS', () => {
  it('matches the schema column names', () => {
    expect(SETTING_KEYS.rainDelayUntil).toBe('rain_delay_until')
    expect(SETTING_KEYS.masterEnabled).toBe('master_enabled')
    expect(SETTING_KEYS.maxZoneSeconds).toBe('max_zone_seconds')
    expect(SETTING_KEYS.logLevel).toBe('log_level')
  })
})

describe('parseCoordinate', () => {
  it.each(['0', '-90', '90', '37.77493', ' 37.5 '])('accepts latitude %s', (text) => {
    expect(parseCoordinate(text, LATITUDE_LIMIT)).toBe(text.trim())
  })

  it.each(['-180', '180', '-122.41942'])('accepts longitude %s', (text) => {
    expect(parseCoordinate(text, LONGITUDE_LIMIT)).toBe(text)
  })

  it.each(['', '90.00001', '+37', '37.', '.5', '3e1', '37.5N', '1234'])('refuses latitude "%s"', (text) => {
    expect(parseCoordinate(text, LATITUDE_LIMIT)).toBeNull()
  })

  it('refuses a longitude past the antimeridian', () => {
    expect(parseCoordinate('-180.5', LONGITUDE_LIMIT)).toBeNull()
  })
})
