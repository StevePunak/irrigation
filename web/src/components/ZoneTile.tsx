import type { Zone } from '../api/types'

export interface ZoneTileProps {
  zone: Zone
  running: boolean
  disabled: boolean
  onRun: (zone: Zone) => void
}

/** A running tile carries no button: its zone's Stop lives in the Running card. */
export default function ZoneTile({ zone, running, disabled, onRun }: ZoneTileProps) {
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
        <span className="zone-tile__running">Running</span>
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
