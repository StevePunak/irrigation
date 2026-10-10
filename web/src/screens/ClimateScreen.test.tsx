import { act, render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import ClimateScreen, { rangeText, toFahrenheit } from './ClimateScreen'
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

describe('climate formatting', () => {
  it('converts Celsius to Fahrenheit', () => {
    expect(toFahrenheit(0)).toBe(32)
    expect(toFahrenheit(100)).toBe(212)
  })

  it('drops the range when it rounds to one value', () => {
    expect(rangeText(70.1, 70.2, 70.4, '%')).toBe('70%')
    expect(rangeText(60, 62, 64, '%')).toBe('62% (60–64)')
  })
})
