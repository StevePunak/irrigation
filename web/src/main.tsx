import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import App from './App'
import './styles/tokens.css'
import './styles/app.css'

const container = document.getElementById('root')

if (container === null) {
  throw new Error('index.html is missing #root')
}

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('/sw.js').catch((error: unknown) => {
      console.error('Service worker registration failed:', error)
    })
  })
}

createRoot(container).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
