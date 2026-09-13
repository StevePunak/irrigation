import { useCallback, useEffect, useState } from 'react'
import { getSettings, getZones, putSettings, putZone } from '../api/client'
import type { SettingsMap, Zone } from '../api/types'
import {
  DEFAULT_MAX_ZONE_SECONDS,
  SETTING_KEYS,
  parseInstant,
  parseInteger,
  parseMasterEnabled,
  serializeMasterEnabled,
} from '../settings/settingsMap'
import { formatDayAndClock } from '../time/zonedformat'
import type { ScreenProps } from './screenProps'

const RAIN_DELAY_CHOICES = [1, 2, 3, 7]

export default function SettingsScreen({ status, refresh }: ScreenProps) {
  const [settings, setSettings] = useState<SettingsMap | null>(null)
  const [zones, setZones] = useState<Zone[]>([])
  const [names, setNames] = useState<Record<number, string>>({})
  const [ceilingMinutes, setCeilingMinutes] = useState(DEFAULT_MAX_ZONE_SECONDS / 60)
  const [error, setError] = useState<string | null>(null)

  const load = useCallback(async () => {
    try {
      const [loadedSettings, loadedZones] = await Promise.all([getSettings(), getZones()])
      setSettings(loadedSettings)
      setZones([...loadedZones].sort((left, right) => left.number - right.number))
      setNames(Object.fromEntries(loadedZones.map((zone) => [zone.number, zone.name])))
      setCeilingMinutes(
        Math.round(parseInteger(loadedSettings[SETTING_KEYS.maxZoneSeconds], DEFAULT_MAX_ZONE_SECONDS) / 60),
      )
      setError(null)
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    }
  }, [])

  useEffect(() => {
    void load()
  }, [load])

  const write = useCallback(
    async (patch: SettingsMap) => {
      setError(null)
      try {
        await putSettings(patch)
        setSettings((current) => (current === null ? current : { ...current, ...patch }))
        refresh()
      } catch (caught: unknown) {
        setError(caught instanceof Error ? caught.message : String(caught))
      }
    },
    [refresh],
  )

  const masterEnabled = settings === null ? null : parseMasterEnabled(settings[SETTING_KEYS.masterEnabled])
  const rainDelayUntil = parseInstant(settings?.[SETTING_KEYS.rainDelayUntil])
  const controllerZone = status?.timezone ?? ''

  const onSaveCeiling = useCallback(() => {
    if (Number.isInteger(ceilingMinutes) === false || ceilingMinutes < 1) {
      setError('The maximum zone runtime must be at least one minute, in whole minutes.')
      return
    }
    void write({ [SETTING_KEYS.maxZoneSeconds]: String(ceilingMinutes * 60) })
  }, [ceilingMinutes, write])

  const onRename = useCallback(
    async (zone: Zone) => {
      setError(null)
      try {
        await putZone(zone, { name: names[zone.number] ?? zone.name, enabled: zone.enabled })
        await load()
      } catch (caught: unknown) {
        setError(caught instanceof Error ? caught.message : String(caught))
      }
    },
    [names, load],
  )

  return (
    <section className="screen">
      <h1>Settings</h1>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      {settings === null ? (
        <p data-testid="settings-unknown">Settings are unknown until the controller answers.</p>
      ) : (
        <>
          <h2>Watering</h2>

          <label>
            Master enable
            <input
              type="checkbox"
              checked={masterEnabled === true}
              onChange={(event) => {
                void write({ [SETTING_KEYS.masterEnabled]: serializeMasterEnabled(event.target.checked) })
              }}
            />
          </label>

          <label>
            Maximum zone runtime (minutes)
            <input
              type="number"
              min={1}
              value={ceilingMinutes}
              onChange={(event) => {
                setCeilingMinutes(Number(event.target.value))
              }}
            />
          </label>
          <button type="button" onClick={onSaveCeiling}>
            Save ceiling
          </button>

          <h2>Rain delay</h2>

          <div data-testid="rain-delay-state">
            {rainDelayUntil === null
              ? 'No rain delay'
              : `Watering paused until ${formatDayAndClock(rainDelayUntil, controllerZone)}`}
          </div>

          <div className="row">
            {RAIN_DELAY_CHOICES.map((days) => (
              <button
                key={days}
                type="button"
                onClick={() => {
                  const until = new Date(Date.now() + days * 86400000).toISOString()
                  void write({ [SETTING_KEYS.rainDelayUntil]: until })
                }}
              >
                {`Delay ${days} ${days === 1 ? 'day' : 'days'}`}
              </button>
            ))}
            <button
              type="button"
              onClick={() => {
                void write({ [SETTING_KEYS.rainDelayUntil]: '' })
              }}
            >
              Clear rain delay
            </button>
          </div>
        </>
      )}

      <h2>Zone names</h2>

      {zones.map((zone) => (
        <div key={zone.number} className="row" data-testid={`zone-row-${zone.number}`}>
          <label>
            {`Zone ${zone.number} name`}
            <input
              type="text"
              value={names[zone.number] ?? ''}
              onChange={(event) => {
                setNames((current) => ({ ...current, [zone.number]: event.target.value }))
              }}
            />
          </label>
          <button
            type="button"
            onClick={() => {
              void onRename(zone)
            }}
          >
            Save
          </button>
        </div>
      ))}
    </section>
  )
}
