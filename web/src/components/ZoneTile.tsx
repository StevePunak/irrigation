import type { Zone } from '../api/types'

export interface ZoneTileProps {
  zone: Zone
  running: boolean
  disabled: boolean
  onRun: (zone: Zone) => void
  onStop: (zoneNumber: number) => void
}

export default function ZoneTile({ zone, running, disabled, onRun, onStop }: ZoneTileProps) {
  return (
    <div
      className="zone-tile"
      data-testid={`zone-tile-${zone.number}`}
      data-running={running ? 'true' : 'false'}
    >
      <span className="zone-tile__number">{zone.number}</span>
      <span className="zone-tile__name">{zone.name}</span>
      {zone.enabled ? null : <span className="zone-tile__off">Disabled</span>}
      {running ? (
        // No disabled state: whoever is standing in the spray must be able to close this valve whatever else is in flight.
        <button
          type="button"
          className="zone-tile__run destructive"
          aria-label={`Stop zone ${zone.number}`}
          onClick={() => {
            onStop(zone.number)
          }}
        >
          Stop
        </button>
      ) : (
        <button
          type="button"
          className="zone-tile__run"
          disabled={disabled || zone.enabled === false}
          onClick={() => {
            onRun(zone)
          }}
        >
          Run
        </button>
      )}
    </div>
  )
}
