import { render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import App, { screenFromHash } from './App'
import * as client from './api/client'
import { idleStatus } from './test/fixtures'

beforeEach(() => {
  window.location.hash = ''
  vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
  vi.spyOn(client, 'getZones').mockResolvedValue([])
  vi.spyOn(client, 'getPrograms').mockResolvedValue([])
  vi.spyOn(client, 'getSettings').mockResolvedValue({})
})

afterEach(() => {
  vi.restoreAllMocks()
})

describe('screenFromHash', () => {
  it('maps the three routes', () => {
    expect(screenFromHash('#/now')).toBe('now')
    expect(screenFromHash('#/programs')).toBe('programs')
    expect(screenFromHash('#/settings')).toBe('settings')
  })

  it('falls back to now for anything else', () => {
    expect(screenFromHash('')).toBe('now')
    expect(screenFromHash('#')).toBe('now')
    expect(screenFromHash('#/nonsense')).toBe('now')
    expect(screenFromHash('#/programs/12')).toBe('now')
  })
})

describe('App', () => {
  it('opens on Now', async () => {
    render(<App />)
    expect(await screen.findByRole('heading', { name: /now/i })).toBeInTheDocument()
  })

  it('navigates by tab and records it in the url', async () => {
    const user = userEvent.setup()
    render(<App />)

    await user.click(screen.getByRole('button', { name: 'Programs' }))

    expect(await screen.findByRole('heading', { name: /programs/i })).toBeInTheDocument()
    expect(window.location.hash).toBe('#/programs')
  })

  it('opens the screen the url names on first render', async () => {
    window.location.hash = '#/settings'
    render(<App />)
    expect(await screen.findByRole('heading', { name: /settings/i })).toBeInTheDocument()
  })

  it('follows a hash change from outside React', async () => {
    render(<App />)
    expect(await screen.findByRole('heading', { name: /now/i })).toBeInTheDocument()

    window.location.hash = '#/settings'
    window.dispatchEvent(new HashChangeEvent('hashchange'))

    expect(await screen.findByRole('heading', { name: /settings/i })).toBeInTheDocument()
  })

  it('marks the active tab for assistive tech', async () => {
    const user = userEvent.setup()
    render(<App />)

    expect(screen.getByRole('button', { name: 'Now' })).toHaveAttribute('aria-current', 'page')

    await user.click(screen.getByRole('button', { name: 'Settings' }))

    expect(screen.getByRole('button', { name: 'Settings' })).toHaveAttribute('aria-current', 'page')
    expect(screen.getByRole('button', { name: 'Now' })).not.toHaveAttribute('aria-current')
  })

  it('shows a banner while the status poll is failing', async () => {
    vi.spyOn(client, 'getStatus').mockRejectedValue(new Error('Failed to fetch'))
    render(<App />)
    expect(await screen.findByRole('status')).toHaveTextContent(/not reaching the controller/i)
  })

  it('does not claim a last known state when the first poll ever fails', async () => {
    vi.spyOn(client, 'getStatus').mockRejectedValue(new Error('Failed to fetch'))
    render(<App />)
    expect(await screen.findByRole('status')).not.toHaveTextContent(/last known state/i)
  })

  it('hides the banner once the poll recovers', async () => {
    vi.spyOn(client, 'getStatus')
      .mockRejectedValueOnce(new Error('Failed to fetch'))
      .mockResolvedValue(idleStatus)

    render(<App />)
    expect(await screen.findByRole('status')).toBeInTheDocument()

    await waitFor(
      () => {
        expect(screen.queryByRole('status')).toBeNull()
      },
      { timeout: 20000 },
    )
  }, 25000)
})
