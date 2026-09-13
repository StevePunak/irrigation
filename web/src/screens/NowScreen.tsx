import type { Status } from '../api/types'

export interface ScreenProps {
  status: Status | null
  /** Identifies the poll `status` came from; the countdown resets when it changes. */
  polls: number
  refresh: () => void
}

export default function NowScreen(_props: ScreenProps) {
  return (
    <section className="screen">
      <h1>Now</h1>
    </section>
  )
}
