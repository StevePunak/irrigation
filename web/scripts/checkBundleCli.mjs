import { checkBundleDir } from './checkBundle.mjs'

const target = process.argv[2] ?? 'dist'
const problems = checkBundleDir(target)
if (problems.length > 0) {
  for (const problem of problems) {
    console.error(`bundle check: ${problem}`)
  }
  process.exit(1)
}
console.log(`bundle check: ${target} is fit to install`)
