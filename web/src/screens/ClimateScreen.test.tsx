import { act, render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import ClimateScreen, { rangeText, toFahrenheit, toInches } from './ClimateScreen'
import * as client from '../api/client'
import { ApiError, type ClimateHistory } from '../api/types'
import { idleStatus } from '../test/fixtures'

const refresh = vi.fn()

const history: ClimateHistory = {
  fromUtc: '2026-10-09T18:00:00Z',
  toUtc: '2026-10-10T18:00:00Z',
  bucketSeconds: 300,
  buckets: [
    {
      startUtc: '2026-10-10T17:50:00Z',
      count: 60,
      temperatureC: { min: 20, mean: 21, max: 22 },
      humidityPercent: { min: 60, mean: 62, max: 64 },
    },
    {
      startUtc: '2026-10-10T17:55:00Z',
      count: 60,
      temperatureC: { min: 25, mean: 25.4, max: 25.6 },
      humidityPercent: { min: 70.2, mean: 70.4, max: 70.6 },
    },
  ],
  weather: { bucketSeconds: 3600, buckets: [] },
}

beforeEach(() => {
  vi.spyOn(client, 'getClimate').mockResolvedValue(history)
})

afterEach(() => {
  vi.restoreAllMocks()
  vi.useRealTimers()
})

describe('ClimateScreen', () => {
  it('asks for the last 24 hours first and reads out the latest bucket', async () => {
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)

    const readout = await screen.findByTestId('climate-readout')
    expect(client.getClimate).toHaveBeenCalledWith(24)
    expect(readout).toHaveTextContent('Latest')
    // 25.4 °C is 77.7 °F; its 25.0–25.6 °C extremes read 77–78.
    expect(readout).toHaveTextContent('78°F (77–78)')
    expect(readout).toHaveTextContent('70%')
  })

  it('draws a temperature chart and a humidity chart', async () => {
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByRole('img', { name: /^Temperature, 68 to 78 °F$/ })).toBeInTheDocument()
    expect(screen.getByRole('img', { name: /^Humidity, 60 to 71 % RH$/ })).toBeInTheDocument()
  })

  it('reloads for the range tapped and marks it pressed', async () => {
    const user = userEvent.setup()
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)
    await screen.findByTestId('climate-readout')

    await user.click(screen.getByRole('button', { name: '7 days' }))

    await waitFor(() => {
      expect(client.getClimate).toHaveBeenLastCalledWith(168)
    })
    expect(screen.getByRole('button', { name: '7 days' })).toHaveAttribute('aria-pressed', 'true')
    expect(screen.getByRole('button', { name: '24 h' })).toHaveAttribute('aria-pressed', 'false')
  })

  it('moves the readout with the arrow keys and shows the crosshair', async () => {
    const user = userEvent.setup()
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)

    const chart = await screen.findByRole('img', { name: /^Temperature/ })
    chart.focus()
    await user.keyboard('{ArrowLeft}')

    const readout = screen.getByTestId('climate-readout')
    // 17:50Z is 10:50 AM in America/Los_Angeles.
    expect(readout).toHaveTextContent('10:50 AM')
    expect(readout).toHaveTextContent('70°F (68–72)')
    expect(screen.getAllByTestId('climate-crosshair')).toHaveLength(2)
  })

  it('lists every bucket newest first in the table view', async () => {
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)
    await screen.findByTestId('climate-readout')

    const rows = within(screen.getByRole('table')).getAllByRole('row')
    expect(rows).toHaveLength(3)
    expect(rows[1]).toHaveTextContent('78')
    expect(rows[2]).toHaveTextContent('68–72')
  })

  it('says so when the range holds no readings', async () => {
    vi.spyOn(client, 'getClimate').mockResolvedValue({ ...history, buckets: [] })
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByText(/no readings in this range yet/i)).toBeInTheDocument()
    expect(screen.queryByRole('img')).toBeNull()
  })

  it('names a missing sensor plainly', async () => {
    vi.spyOn(client, 'getClimate').mockRejectedValue(new ApiError(404, 'no climate sensor is configured'))
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByRole('alert')).toHaveTextContent('No climate sensor is configured.')
  })

  it('refreshes every minute', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true })
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)
    await screen.findByTestId('climate-readout')
    expect(client.getClimate).toHaveBeenCalledTimes(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(60000)
    })
    expect(client.getClimate).toHaveBeenCalledTimes(2)
  })
})

const withWeather: ClimateHistory = {
  ...history,
  weather: {
    bucketSeconds: 3600,
    buckets: [
      { startUtc: '2026-10-10T16:00:00Z', precipitationMm: 2.54, et0Mm: 0.254, temperatureC: 20, humidityPercent: 80 },
      { startUtc: '2026-10-10T17:00:00Z', precipitationMm: 5.08, et0Mm: 0.508, temperatureC: 21, humidityPercent: 75 },
      { startUtc: '2026-10-10T18:00:00Z', precipitationMm: null, et0Mm: null, temperatureC: 22, humidityPercent: 70 },
    ],
  },
}

describe('ClimateScreen with Open-Meteo weather', () => {
  beforeEach(() => {
    vi.spyOn(client, 'getClimate').mockResolvedValue(withWeather)
  })

  it('totals the range in the rain chart', async () => {
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(
      await screen.findByRole('img', { name: 'Rain and ET₀ from Open-Meteo: 0.30 in of rain and 0.03 in of ET₀ in this range' }),
    ).toBeInTheDocument()
  })

  it('reads out the newest value of each measure while nothing is hovered', async () => {
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)

    const weather = await screen.findByTestId('climate-readout-weather')
    // 22 °C is 71.6 °F; the 18:00 hour has not finished, so rain and ET₀ come from 17:00.
    expect(weather).toHaveTextContent('Open-Meteo 72°F · 70% · rain 0.20 in · ET₀ 0.02 in')
  })

  it('draws Open-Meteo as a labelled reference on both sensor charts', async () => {
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)
    await screen.findByRole('img', { name: /^Temperature/ })

    expect(screen.getAllByText('Open-Meteo')).toHaveLength(2)
    expect(screen.getAllByText('Sensor')).toHaveLength(2)
  })

  it('moves both crosshairs from the rain chart', async () => {
    const user = userEvent.setup()
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)

    const rain = await screen.findByRole('img', { name: /^Rain and ET₀/ })
    rain.focus()
    await user.keyboard('{Home}')

    expect(screen.getByTestId('weather-crosshair')).toBeInTheDocument()
    expect(screen.getAllByTestId('climate-crosshair')).toHaveLength(2)
    expect(screen.getByTestId('climate-readout-weather')).toHaveTextContent('rain 0.10 in')
  })

  it('explains an empty weather range', async () => {
    vi.spyOn(client, 'getClimate').mockResolvedValue(history)
    render(<ClimateScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByTestId('weather-empty')).toHaveTextContent(/location on the Settings page/)
    expect(screen.queryByText('Open-Meteo')).toBeNull()
  })
})

describe('climate formatting', () => {
  it('converts Celsius to Fahrenheit', () => {
    expect(toFahrenheit(0)).toBe(32)
    expect(toFahrenheit(100)).toBe(212)
  })

  it('converts millimetres to inches', () => {
    expect(toInches(25.4)).toBe(1)
  })

  it('drops the range when it rounds to one value', () => {
    expect(rangeText(70.1, 70.2, 70.4, '%')).toBe('70%')
    expect(rangeText(60, 62, 64, '%')).toBe('62% (60–64)')
  })
})
