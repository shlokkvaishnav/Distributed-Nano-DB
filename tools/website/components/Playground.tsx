'use client'

import { useState, useEffect, useCallback } from 'react'

const API_BASE = process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8080'

function generateRandomVector(dim: number): number[] {
  return Array.from({ length: dim }, () => Math.round(Math.random() * 1000) / 1000)
}

export default function Playground() {
  const [insertCount, setInsertCount] = useState(100)
  const [metric, setMetric] = useState<'L2' | 'Cosine' | 'InnerProduct'>('L2')
  const [inserting, setInserting] = useState(false)
  const [insertResult, setInsertResult] = useState<string | null>(null)

  const [queryVector, setQueryVector] = useState('')
  const [k, setK] = useState(5)
  const [searching, setSearching] = useState(false)
  const [searchResults, setSearchResults] = useState<Array<{ id: number; distance: number; metadata?: string }>>([])

  const [stats, setStats] = useState<{ element_count: number; vector_dim: number; metric: string } | null>(null)
  const [connected, setConnected] = useState(false)
  const [tps, setTps] = useState(0)

  const fetchStats = useCallback(async () => {
    try {
      const res = await fetch(`${API_BASE}/stats`)
      if (res.ok) {
        const data = await res.json()
        setStats(data)
        setConnected(true)
      } else {
        setConnected(false)
      }
    } catch {
      setConnected(false)
    }
  }, [])

  useEffect(() => {
    fetchStats()
    const interval = setInterval(fetchStats, 2000)
    return () => clearInterval(interval)
  }, [fetchStats])

  async function handleInsert() {
    setInserting(true)
    setInsertResult(null)
    const start = performance.now()
    let success = 0

    for (let i = 0; i < insertCount; i++) {
      try {
        const res = await fetch(`${API_BASE}/vectors`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            id: Date.now() + i,
            vector: generateRandomVector(128),
            metadata: `vec_${i}`,
          }),
        })
        if (res.ok) success++
      } catch {
        break
      }
    }

    const elapsed = (performance.now() - start) / 1000
    const measuredTps = Math.round(success / elapsed)
    setTps(measuredTps)
    setInsertResult(`Inserted ${success}/${insertCount} vectors in ${elapsed.toFixed(2)}s (${measuredTps} TPS)`)
    setInserting(false)
    fetchStats()
  }

  async function handleSearch() {
    setSearching(true)
    setSearchResults([])

    let vec: number[]
    if (queryVector.trim()) {
      try {
        vec = JSON.parse(queryVector)
      } catch {
        setSearchResults([])
        setSearching(false)
        return
      }
    } else {
      vec = generateRandomVector(128)
      setQueryVector(JSON.stringify(vec.slice(0, 5)) + '... (128d random)')
    }

    try {
      const res = await fetch(`${API_BASE}/search`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ vector: vec, k }),
      })
      if (res.ok) {
        const data = await res.json()
        setSearchResults(data.results || data)
      }
    } catch {
      setSearchResults([])
    }
    setSearching(false)
  }

  return (
    <section id="playground" className="py-20 px-6 border-t border-nano-border">
      <div className="max-w-6xl mx-auto">
        <h2 className="text-2xl font-bold text-center mb-2">Live Playground</h2>
        <p className="text-nano-muted text-center text-sm mb-10">
          Connected to a live NanoDB instance. Insert vectors, search, and watch stats update in real time.
        </p>

        <div className="grid grid-cols-1 lg:grid-cols-3 gap-6">
          {/* Insert Panel */}
          <div className="bg-nano-surface border border-nano-border rounded-lg p-6">
            <h3 className="text-sm font-semibold text-nano-accent uppercase tracking-wider mb-4">
              Insert Vectors
            </h3>

            <label className="block text-xs text-nano-muted mb-1">Count: {insertCount}</label>
            <input
              type="range"
              min={10}
              max={10000}
              step={10}
              value={insertCount}
              onChange={(e) => setInsertCount(Number(e.target.value))}
              className="w-full mb-4 accent-[#00d4aa]"
            />

            <label className="block text-xs text-nano-muted mb-1">Distance Metric</label>
            <select
              value={metric}
              onChange={(e) => setMetric(e.target.value as typeof metric)}
              className="w-full bg-nano-bg border border-nano-border rounded px-3 py-2 text-sm text-nano-text mb-4 focus:outline-none focus:border-nano-accent"
            >
              <option value="L2">L2 (Euclidean)</option>
              <option value="Cosine">Cosine</option>
              <option value="InnerProduct">Inner Product</option>
            </select>

            <button
              onClick={handleInsert}
              disabled={inserting || !connected}
              className="w-full py-2.5 bg-nano-accent text-nano-bg font-semibold rounded text-sm hover:bg-nano-accent/90 transition-colors disabled:opacity-50 disabled:cursor-not-allowed"
            >
              {inserting ? 'Inserting...' : `Insert ${insertCount} Vectors`}
            </button>

            {insertResult && (
              <div className="mt-3 text-xs text-nano-accent bg-nano-accent-dim rounded px-3 py-2 fade-in">
                {insertResult}
              </div>
            )}
          </div>

          {/* Search Panel */}
          <div className="bg-nano-surface border border-nano-border rounded-lg p-6">
            <h3 className="text-sm font-semibold text-nano-accent uppercase tracking-wider mb-4">
              Search
            </h3>

            <label className="block text-xs text-nano-muted mb-1">Query Vector (JSON array or empty for random)</label>
            <textarea
              value={queryVector}
              onChange={(e) => setQueryVector(e.target.value)}
              placeholder="[0.1, 0.2, ...] or leave empty"
              className="w-full bg-nano-bg border border-nano-border rounded px-3 py-2 text-xs text-nano-text h-20 resize-none mb-3 font-mono focus:outline-none focus:border-nano-accent"
            />

            <label className="block text-xs text-nano-muted mb-1">k (neighbors): {k}</label>
            <input
              type="range"
              min={1}
              max={50}
              value={k}
              onChange={(e) => setK(Number(e.target.value))}
              className="w-full mb-4 accent-[#00d4aa]"
            />

            <button
              onClick={handleSearch}
              disabled={searching || !connected}
              className="w-full py-2.5 border border-nano-accent text-nano-accent font-semibold rounded text-sm hover:bg-nano-accent/10 transition-colors disabled:opacity-50 disabled:cursor-not-allowed"
            >
              {searching ? 'Searching...' : 'Search'}
            </button>

            {searchResults.length > 0 && (
              <div className="mt-3 max-h-40 overflow-y-auto fade-in">
                {searchResults.map((r, i) => (
                  <div
                    key={i}
                    className="flex justify-between text-xs py-1.5 border-b border-nano-border last:border-0"
                  >
                    <span className="text-nano-text">id: {r.id}</span>
                    <span className="text-nano-muted">d: {r.distance.toFixed(4)}</span>
                  </div>
                ))}
              </div>
            )}
          </div>

          {/* Stats Panel */}
          <div className="bg-nano-surface border border-nano-border rounded-lg p-6">
            <h3 className="text-sm font-semibold text-nano-accent uppercase tracking-wider mb-4">
              Live Stats
            </h3>

            <div className="flex items-center gap-2 mb-4">
              <div className={`w-2 h-2 rounded-full ${connected ? 'bg-green-400' : 'bg-red-400'}`} />
              <span className="text-xs text-nano-muted">
                {connected ? 'Connected' : 'Disconnected'}
              </span>
            </div>

            {stats ? (
              <div className="space-y-3">
                <div className="flex justify-between items-center">
                  <span className="text-xs text-nano-muted">Vectors</span>
                  <span className="text-lg font-bold text-nano-text">{stats.element_count.toLocaleString()}</span>
                </div>
                <div className="flex justify-between items-center">
                  <span className="text-xs text-nano-muted">Dimensions</span>
                  <span className="text-lg font-bold text-nano-text">{stats.vector_dim}</span>
                </div>
                <div className="flex justify-between items-center">
                  <span className="text-xs text-nano-muted">Metric</span>
                  <span className="text-lg font-bold text-nano-text">{stats.metric}</span>
                </div>
                <div className="border-t border-nano-border pt-3 mt-3">
                  <div className="flex justify-between items-center">
                    <span className="text-xs text-nano-muted">Last Measured TPS</span>
                    <span className="text-lg font-bold text-nano-accent stat-glow">
                      {tps > 0 ? tps.toLocaleString() : '—'}
                    </span>
                  </div>
                </div>
              </div>
            ) : (
              <div className="text-xs text-nano-muted">
                {connected ? 'Loading...' : 'Start the NanoDB server to connect.'}
              </div>
            )}

            <div className="mt-6 p-3 bg-nano-bg rounded border border-nano-border">
              <p className="text-[10px] text-nano-muted leading-relaxed font-mono">
                $ docker run -p 8080:8080 \<br />
                &nbsp;&nbsp;ghcr.io/shlokkvaishnav/nano-db
              </p>
            </div>
          </div>
        </div>
      </div>
    </section>
  )
}
