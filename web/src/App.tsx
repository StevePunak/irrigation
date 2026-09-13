import { useCallback, useEffect, useState } from 'react'
import { useStatus } from './hooks/useStatus'
import NowScreen from './screens/NowScreen'
import ProgramsScreen from './screens/ProgramsScreen'
import SettingsScreen from './screens/SettingsScreen'

export type ScreenName = 'now' | 'programs' | 'settings'

const TABS: { name: ScreenName; label: string }[] = [
  { name: 'now', label: 'Now' },
  { name: 'programs', label: 'Programs' },
  { name: 'settings', label: 'Settings' },
]

/** Maps a URL fragment to a screen. Anything unrecognised lands on Now. */
export function screenFromHash(hash: string): ScreenName {
  const route = hash.replace(/^#\/?/, '')
  const match = TABS.find((tab) => tab.name === route)
  return match === undefined ? 'now' : match.name
}

export default function App() {
  const [screen, setScreen] = useState<ScreenName>(() => screenFromHash(window.location.hash))
  const { status, error, stale, polls, refresh } = useStatus()

  useEffect(() => {
    const onHashChange = () => {
      setScreen(screenFromHash(window.location.hash))
    }
    window.addEventListener('hashchange', onHashChange)
    return () => {
      window.removeEventListener('hashchange', onHashChange)
    }
  }, [])

  const navigate = useCallback((name: ScreenName) => {
    window.location.hash = `#/${name}`
    setScreen(name)
  }, [])

  const props = { status, polls, refresh }

  return (
    <div className="app">
      {stale ? (
        <div className="banner" role="status">
          Not reaching the controller — showing the last known state. {error}
        </div>
      ) : null}

      {screen === 'now' ? <NowScreen {...props} /> : null}
      {screen === 'programs' ? <ProgramsScreen {...props} /> : null}
      {screen === 'settings' ? <SettingsScreen {...props} /> : null}

      <nav className="tabs">
        {TABS.map((tab) => (
          <button
            key={tab.name}
            type="button"
            {...(screen === tab.name ? { 'aria-current': 'page' as const } : {})}
            onClick={() => {
              navigate(tab.name)
            }}
          >
            {tab.label}
          </button>
        ))}
      </nav>
    </div>
  )
}
