import { useCallback, useEffect, useState } from 'react'
import { getSettings, getZones, putSettings, putZone } from '../api/client'
import type { SettingsMap, Zone } from '../api/types'
import {
  DEFAULT_MAX_ZONE_SECONDS,
  DEFAULT_MAX_CONCURRENT_ZONES,
  DEFAULT_PANEL_RUN_MINUTES,
  MAX_CONCURRENT_ZONES_LIMIT,
  PANEL_RUN_MINUTES_LIMIT,
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
  const [maxZones, setMaxZones] = useState(DEFAULT_MAX_CONCURRENT_ZONES)
  const [panelMinutes, setPanelMinutes] = useState(DEFAULT_PANEL_RUN_MINUTES)
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
      setMaxZones(parseInteger(loadedSettings[SETTING_KEYS.maxConcurrentZones], DEFAULT_MAX_CONCURRENT_ZONES))
      setPanelMinutes(parseInteger(loadedSettings[SETTING_KEYS.panelRunMinutes], DEFAULT_PANEL_RUN_MINUTES))
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

  const onSaveMaxZones = useCallback(() => {
    if (Number.isInteger(maxZones) === false || maxZones < 1 || maxZones > MAX_CONCURRENT_ZONES_LIMIT) {
      setError(`Max zones at once must be a whole number from 1 to ${MAX_CONCURRENT_ZONES_LIMIT}.`)
      return
    }
    void write({ [SETTING_KEYS.maxConcurrentZones]: String(maxZones) })
  }, [maxZones, write])

  const onSavePanelMinutes = useCallback(() => {
    if (Number.isInteger(panelMinutes) === false || panelMinutes < 1 || panelMinutes > PANEL_RUN_MINUTES_LIMIT) {
      setError(`The gardener panel run time must be a whole number of minutes from 1 to ${PANEL_RUN_MINUTES_LIMIT}.`)
      return
    }
    void write({ [SETTING_KEYS.panelRunMinutes]: String(panelMinutes) })
  }, [panelMinutes, write])

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

          <label>
            Max zones at once
            <input
              type="number"
              min={1}
              max={MAX_CONCURRENT_ZONES_LIMIT}
              value={maxZones}
              onChange={(event) => {
                setMaxZones(Number(event.target.value))
              }}
            />
          </label>
          <button type="button" onClick={onSaveMaxZones}>
            Save max zones
          </button>

          <label>
            Gardener panel run time (minutes)
            <input
              type="number"
              min={1}
              max={PANEL_RUN_MINUTES_LIMIT}
              value={panelMinutes}
              onChange={(event) => {
                setPanelMinutes(Number(event.target.value))
              }}
            />
          </label>
          <button type="button" onClick={onSavePanelMinutes}>
            Save panel run time
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
