export interface StopButtonProps {
  onStop: () => void
  busy: boolean
}

/**
 * No confirmation step and no disabled state driven by connectivity. Both are
 * things a maintainer adds while tidying, and both break the control for the
 * person standing in the spray.
 */
export default function StopButton({ onStop, busy }: StopButtonProps) {
  return (
    <button type="button" className="stop" onClick={onStop} aria-busy={busy}>
      STOP
    </button>
  )
}
