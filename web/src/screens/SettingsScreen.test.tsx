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

  it('shows no rain delay for an empty value', async () => {
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

  it('saves max zones at once as a string', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/max zones at once/i)
    expect(field).toHaveValue(2)
    await user.clear(field)
    await user.type(field, '3')
    await user.click(screen.getByRole('button', { name: 'Save max zones' }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ max_concurrent_zones: '3' })
    })
  })

  it('refuses max zones outside 1 to 8', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/max zones at once/i)
    await user.clear(field)
    await user.type(field, '9')
    await user.click(screen.getByRole('button', { name: 'Save max zones' }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/1 to 8/)
    expect(putSettings).not.toHaveBeenCalled()
  })

  it('shows a gardener panel run time of 10 minutes when the setting is absent', async () => {
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByLabelText(/gardener panel run time/i)).toHaveValue(10)
  })

  it('reads the stored gardener panel run time', async () => {
    vi.spyOn(client, 'getSettings').mockResolvedValue({ master_enabled: '1', panel_run_minutes: '25' })
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByLabelText(/gardener panel run time/i)).toHaveValue(25)
  })

  it('saves the gardener panel run time as a string', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/gardener panel run time/i)
    await user.clear(field)
    await user.type(field, '15')
    await user.click(screen.getByRole('button', { name: 'Save panel run time' }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ panel_run_minutes: '15' })
    })
  })

  it.each(['0', '61'])('refuses a gardener panel run time of %s', async (value) => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/gardener panel run time/i)
    await user.clear(field)
    await user.type(field, value)
    await user.click(screen.getByRole('button', { name: 'Save panel run time' }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/1 to 60/)
    expect(putSettings).not.toHaveBeenCalled()
  })

  it('refuses a gardener panel run time that is not a whole number of minutes', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/gardener panel run time/i)
    await user.clear(field)
    await user.type(field, '12.5')
    await user.click(screen.getByRole('button', { name: 'Save panel run time' }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/whole number/i)
    expect(putSettings).not.toHaveBeenCalled()
  })

  it.each(['1', '60'])('accepts a gardener panel run time of %s', async (value) => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/gardener panel run time/i)
    await user.clear(field)
    await user.type(field, value)
    await user.click(screen.getByRole('button', { name: 'Save panel run time' }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ panel_run_minutes: value })
    })
  })

  it('shows no settings values when they could not be loaded', async () => {
    vi.spyOn(client, 'getSettings').mockRejectedValue(new Error('controller unreachable'))

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByRole('alert')).toHaveTextContent(/controller unreachable/i)
    expect(screen.getByTestId('settings-unknown')).toBeInTheDocument()
    expect(screen.queryByLabelText(/master enable/i)).toBeNull()
    expect(screen.queryByLabelText(/maximum zone runtime/i)).toBeNull()
    expect(screen.queryByText(/no rain delay/i)).toBeNull()
  })

  it('refuses a ceiling that is not a whole number of minutes', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const field = await screen.findByLabelText(/maximum zone runtime/i)
    await user.clear(field)
    await user.type(field, '1.25')
    await user.click(screen.getByRole('button', { name: /save ceiling/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/whole minutes/i)
    expect(putSettings).not.toHaveBeenCalled()
  })

  it('reports a rejected write', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    vi.spyOn(client, 'putSettings').mockRejectedValue(new Error('database is locked'))

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByLabelText(/master enable/i))

    expect(await screen.findByRole('alert')).toHaveTextContent(/database is locked/i)
  })

  it('shows the stored location', async () => {
    vi.spyOn(client, 'getSettings').mockResolvedValue({ latitude: '37.77493', longitude: '-122.41942' })
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByLabelText(/^latitude/i)).toHaveValue('37.77493')
    expect(screen.getByLabelText(/^longitude/i)).toHaveValue('-122.41942')
  })

  it('saves both coordinates in one write', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.type(await screen.findByLabelText(/^latitude/i), ' 37.77493 ')
    await user.type(screen.getByLabelText(/^longitude/i), '-122.41942')
    await user.click(screen.getByRole('button', { name: 'Save location' }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ latitude: '37.77493', longitude: '-122.41942' })
    })
  })

  it('clears the location when both fields are empty', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    vi.spyOn(client, 'getSettings').mockResolvedValue({ latitude: '37.77493', longitude: '-122.41942' })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.clear(await screen.findByLabelText(/^latitude/i))
    await user.clear(screen.getByLabelText(/^longitude/i))
    await user.click(screen.getByRole('button', { name: 'Save location' }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ latitude: '', longitude: '' })
    })
  })

  it.each([
    ['only a latitude', '37.77493', ''],
    ['a latitude past the pole', '91', '-122.41942'],
    ['a longitude past the antimeridian', '37.77493', '-181'],
    ['compass letters', '37.77493N', '122.41942W'],
  ])('refuses %s', async (_label, latitude, longitude) => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    if (latitude !== '') {
      await user.type(await screen.findByLabelText(/^latitude/i), latitude)
    }
    if (longitude !== '') {
      await user.type(await screen.findByLabelText(/^longitude/i), longitude)
    }
    await user.click(await screen.findByRole('button', { name: 'Save location' }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/decimal degrees/i)
    expect(putSettings).not.toHaveBeenCalled()
  })

  it('hides the device-location button outside a secure context', async () => {
    Object.defineProperty(window, 'isSecureContext', { configurable: true, value: false })
    Object.defineProperty(navigator, 'geolocation', { configurable: true, value: { getCurrentPosition: vi.fn() } })
    try {
      render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

      await screen.findByLabelText(/^latitude/i)
      expect(screen.queryByRole('button', { name: /use this device/i })).toBeNull()
    } finally {
      Reflect.deleteProperty(navigator, 'geolocation')
      Reflect.deleteProperty(window, 'isSecureContext')
    }
  })

  it('fills the fields from the device location, rounded to five places', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    Object.defineProperty(window, 'isSecureContext', { configurable: true, value: true })
    const getCurrentPosition = vi.fn((success: PositionCallback) => {
      success({ coords: { latitude: 37.774929123, longitude: -122.419415678 } } as GeolocationPosition)
    })
    Object.defineProperty(navigator, 'geolocation', { configurable: true, value: { getCurrentPosition } })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    try {
      render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
      await user.click(await screen.findByRole('button', { name: /use this device/i }))

      expect(screen.getByLabelText(/^latitude/i)).toHaveValue('37.77493')
      expect(screen.getByLabelText(/^longitude/i)).toHaveValue('-122.41942')
      expect(putSettings).not.toHaveBeenCalled()
    } finally {
      Reflect.deleteProperty(navigator, 'geolocation')
      Reflect.deleteProperty(window, 'isSecureContext')
    }
  })

  it('rounds a long coordinate to five places before saving it', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.type(await screen.findByLabelText(/^latitude/i), '33.463487535940224')
    await user.type(screen.getByLabelText(/^longitude/i), '-117.65823197849812')
    await user.click(screen.getByRole('button', { name: 'Save location' }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ latitude: '33.46349', longitude: '-117.65823' })
    })
    expect(screen.getByLabelText(/^latitude/i)).toHaveValue('33.46349')
    expect(screen.getByLabelText(/^longitude/i)).toHaveValue('-117.65823')
  })

  it('shows each save as done until its field changes', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    vi.spyOn(client, 'getSettings').mockResolvedValue({
      max_zone_seconds: '1800',
      max_concurrent_zones: '2',
      panel_run_minutes: '10',
      latitude: '33.46349',
      longitude: '-117.65823',
    })

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    for (const name of ['Ceiling saved ✓', 'Max zones saved ✓', 'Panel run time saved ✓', 'Location saved ✓']) {
      expect(await screen.findByRole('button', { name })).toBeDisabled()
    }

    const field = screen.getByLabelText(/gardener panel run time/i)
    await user.clear(field)
    await user.type(field, '15')
    expect(screen.getByRole('button', { name: 'Save panel run time' })).toBeEnabled()

    await user.click(screen.getByRole('button', { name: 'Save panel run time' }))
    expect(await screen.findByRole('button', { name: 'Panel run time saved ✓' })).toBeDisabled()
  })

  it('keeps a failed save offered', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    vi.spyOn(client, 'putSettings').mockRejectedValue(new Error('database is locked'))

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.type(await screen.findByLabelText(/^latitude/i), '33.46349')
    await user.type(screen.getByLabelText(/^longitude/i), '-117.65823')
    await user.click(screen.getByRole('button', { name: 'Save location' }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/database is locked/i)
    expect(screen.getByRole('button', { name: 'Save location' })).toBeEnabled()
  })
})
