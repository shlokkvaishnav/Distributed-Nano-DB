import type { Metadata } from 'next'
import './globals.css'

export const metadata: Metadata = {
  title: 'NanoDB — Vector Search Engine',
  description: 'Sub-millisecond ANN search. Zero external dependencies. Runs in Docker.',
}

export default function RootLayout({
  children,
}: {
  children: React.ReactNode
}) {
  return (
    <html lang="en">
      <body className="antialiased">{children}</body>
    </html>
  )
}
