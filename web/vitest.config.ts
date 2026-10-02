import react from '@vitejs/plugin-react'
import { defineConfig } from 'vitest/config'

export default defineConfig({
  plugins: [react()],
  test: {
    globals: true,
    environment: 'jsdom',
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.test.{ts,tsx}', 'scripts/**/*.test.mjs'],
    exclude: [
      '**/node_modules/**',
      '**/dist/**',
      'src/**/*.tz.test.ts',
      'src/api/decode.test.ts',
      'src/api/client.test.ts',
      'src/programs/dayRule.test.ts',
      'src/screens/ProgramEditor.test.tsx',
      'src/screens/ProgramsScreen.test.tsx',
    ],
    restoreMocks: true,
  },
})
