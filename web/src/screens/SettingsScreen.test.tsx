import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import SettingsScreen from './SettingsScreen'
import * as client from '../api/client'
import { idleStatus, zoneFixtures } from '../test/fixtures'

const refresh = vi.fn()

beforeEach(() => {
  vi.useFakeTimers({ shouldAdvanceTime: true })
  vi.setSystemTime(new Date('2026-09-13T20:00:00Z'))
  vi.spyOn(client, 'getZones').mockResolvedValue(zoneFixtures)
  vi.spyOn(client, 'putZone').mockResolvedValue(undefined)
  vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
  vi.spyOn(client, 'getSettings').mockResolvedValue({
    rain_delay_until: '',
    master_enabled: '1',
    max_zone_seconds: '1800',
    log_level: 'info',
  })
})

afterEach(() => {
  vi.useRealTimers()
  vi.restoreAllMocks()
  refresh.mockReset()
})

describe('SettingsScreen', () => {
  it('shows the current values', async () => {
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByLabelText(/master enable/i)).toBeChecked()
    expect(screen.getByLabelText(/maximum zone runtime/i)).toHaveValue(30)
    expect(screen.getByTestId('rain-delay-state')).toHaveTextContent(/no rain delay/i)
  })

  it('reads "0" as disabled', async () => {
    vi.spyOn(client, 'getSettings').mockResolvedValue({
      master_enabled: '0',
      max_zone_seconds: '1800',
    })

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByLabelText(/master enable/i)).not.toBeChecked()
  })

  it('falls back to 60 minutes when the ceiling is missing', async () => {
    vi.spyOn(client, 'getSettings').mockResolvedValue({ master_enabled: '1' })
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByLabelText(/maximum zone runtime/i)).toHaveValue(60)
  })

  it('sends the master enable as "0" or "1"', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByLabelText(/master enable/i))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ master_enabled: '0' })
    })
  })

  it('sends the run ceiling in seconds as a string', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const field = await screen.findByLabelText(/maximum zone runtime/i)
    await user.clear(field)
    await user.type(field, '15')
    await user.click(screen.getByRole('button', { name: /save ceiling/i }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ max_zone_seconds: '900' })
    })
  })

  it('refuses a zero or negative ceiling', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const field = await screen.findByLabelText(/maximum zone runtime/i)
    await user.clear(field)
    await user.type(field, '0')
    await user.click(screen.getByRole('button', { name: /save ceiling/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/at least one minute/i)
    expect(putSettings).not.toHaveBeenCalled()
  })

  it('sets a rain delay as a UTC instant the requested number of days out', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByRole('button', { name: /delay 2 days/i }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledTimes(1)
    })
    const sent = putSettings.mock.calls[0]![0]
    expect(Object.keys(sent)).toEqual(['rain_delay_until'])
    expect(sent.rain_delay_until).toMatch(/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/)
    const offset = Date.parse(sent.rain_delay_until!) - Date.parse('2026-09-15T20:00:00Z')
    expect(offset).toBeGreaterThanOrEqual(0)
    expect(offset).toBeLessThan(60_000)
  })

  it('clears a rain delay with an empty string', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    vi.spyOn(client, 'getSettings').mockResolvedValue({
      rain_delay_until: '2026-09-15T20:00:00Z',
      master_enabled: '1',
      max_zone_seconds: '1800',
    })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByRole('button', { name: /clear rain delay/i }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ rain_delay_until: '' })
    })
  })

  it('renders an active rain delay in the controller zone', async () => {
    vi.spyOn(client, 'getSettings').mockResolvedValue({
      rain_delay_until: '2026-09-15T20:00:00Z',
      master_enabled: '1',
      max_zone_seconds: '1800',
    })

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    // 20:00 UTC is 1:00 PM in America/Los_Angeles.
    expect(await screen.findByTestId('rain-delay-state')).toHaveTextContent('1:00 PM')
  })

  it('shows no rain delay for an empty value rather than an invalid date', async () => {
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const state = await screen.findByTestId('rain-delay-state')
    expect(state).toHaveTextContent(/no rain delay/i)
    expect(state).not.toHaveTextContent(/invalid/i)
    expect(state).not.toHaveTextContent(/NaN/)
  })

  it('renames a zone by its zone number', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putZone = vi.spyOn(client, 'putZone').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const row = await screen.findByTestId('zone-row-3')
    const field = within(row).getByLabelText(/zone 3 name/i)
    await user.clear(field)
    await user.type(field, 'Rose bed')
    await user.click(within(row).getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(putZone).toHaveBeenCalledTimes(1)
    })
    const [zoneArgument, patch] = putZone.mock.calls[0]!
    expect(zoneArgument.number).toBe(3)
    expect(zoneArgument.id).toBe(9)
    expect(patch).toEqual({ name: 'Rose bed', enabled: true })
  })

  it('carries the enabled flag through a rename', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putZone = vi.spyOn(client, 'putZone').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    // zoneFixtures[6] is zone number 7 with enabled: false.
    const row = await screen.findByTestId('zone-row-7')
    await user.clear(within(row).getByLabelText(/zone 7 name/i))
    await user.type(within(row).getByLabelText(/zone 7 name/i), 'Pots')
    await user.click(within(row).getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(putZone).toHaveBeenCalled()
    })
    expect(putZone.mock.calls[0]![1]).toEqual({ name: 'Pots', enabled: false })
  })

  it('reports a rejected write', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    vi.spyOn(client, 'putSettings').mockRejectedValue(new Error('database is locked'))

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByLabelText(/master enable/i))

    expect(await screen.findByRole('alert')).toHaveTextContent(/database is locked/i)
  })
})
