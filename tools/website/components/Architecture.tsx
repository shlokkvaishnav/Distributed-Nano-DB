'use client'

import { useState } from 'react'

interface ArchCard {
  title: string
  subtitle: string
  diagram: string
  explanation: string
}

const cards: ArchCard[] = [
  {
    title: 'MMap Storage Engine',
    subtitle: 'Zero-copy persistence via mmap()',
    diagram: `
┌─────────────────────────────────────────────┐
│            Virtual Address Space             │
├───────────┬─────────┬─────────┬─────────────┤
│  Header   │ Node 0  │ Node 1  │   Node N    │
│  64 bytes │ ~2.2KB  │ ~2.2KB  │   ~2.2KB    │
├───────────┴─────────┴─────────┴─────────────┤
│                                             │
│          mmap() ← file on disk              │
│     Pages loaded lazily by OS on access     │
│     No explicit read/write syscalls         │
│                                             │
└─────────────────────────────────────────────┘`,
    explanation:
      'The storage engine maps the entire index file into virtual memory with mmap(). The OS loads pages on demand — a 100GB index works on an 8GB machine because only accessed pages reside in RAM. Writes flush to disk via msync(). Zero deserialization on startup: the file IS the data structure.',
  },
  {
    title: 'HNSW Indexing',
    subtitle: 'O(log N) approximate nearest neighbors',
    diagram: `
Layer 2:   [A] ─────────────────────── [D]
            │                            │
Layer 1:   [A] ──── [B] ──── [C] ──── [D]
            │        │        │         │
Layer 0:   [A]─[E]─[B]─[F]─[C]─[G]─[D]─[H]

Search: Enter at top layer → greedy descend
        → beam search at layer 0 (ef=100)

Insert: Random level ← geometric(p=0.03)
        → connect to M nearest per layer`,
    explanation:
      'HNSW builds a multi-layer skip-list graph. Higher layers have exponentially fewer nodes, enabling O(log N) traversal. Search enters at the top layer, greedily descends, then does a beam search (ef candidates) at layer 0. Each node stores up to M=16 neighbors per layer (M_MAX0=32 at layer 0). The probabilistic layer assignment (p=0.03) naturally creates the logarithmic hierarchy.',
  },
  {
    title: 'Stripe Lock Concurrency',
    subtitle: 'One SpinLock per node — no global lock on insert',
    diagram: `
Thread 1 ───► lock(Node 5) ──► add_link ──► unlock
Thread 2 ───► lock(Node 12) ─► add_link ──► unlock
Thread 3 ───► lock(Node 5) ──► [waits...] ► add_link ► unlock
Thread 4 ───► lock(Node 99) ─► add_link ──► unlock

    ┌──────────────────────────────────────┐
    │  Only global lock: resize_lock_      │
    │  (triggered ~1 time per 5000 inserts │
    │   when mmap file needs expansion)    │
    └──────────────────────────────────────┘`,
    explanation:
      'Each node has its own SpinLock — inserting node A only blocks threads also writing to node A. This yields near-linear scaling up to memory bandwidth limits (2.88x at 8 threads). SpinLocks outperform std::mutex for these ~100ns critical sections because they avoid kernel transitions. The only global lock fires during rare storage resizes (~10MB expansion events).',
  },
]

export default function Architecture() {
  const [expanded, setExpanded] = useState<number | null>(null)

  return (
    <section id="architecture" className="py-20 px-6 border-t border-nano-border">
      <div className="max-w-6xl mx-auto">
        <h2 className="text-2xl font-bold text-center mb-2">Architecture Deep Dive</h2>
        <p className="text-nano-muted text-center text-sm mb-10">
          Click a card to see the internal design.
        </p>

        <div className="grid grid-cols-1 md:grid-cols-3 gap-6">
          {cards.map((card, i) => (
            <div
              key={i}
              onClick={() => setExpanded(expanded === i ? null : i)}
              className={`bg-nano-surface border rounded-lg p-6 cursor-pointer transition-all ${
                expanded === i
                  ? 'border-nano-accent glow md:col-span-3'
                  : 'border-nano-border hover:border-nano-accent/50'
              }`}
            >
              <h3 className="text-base font-semibold text-nano-text mb-1">{card.title}</h3>
              <p className="text-xs text-nano-muted">{card.subtitle}</p>

              {expanded === i && (
                <div className="mt-6 fade-in">
                  <pre className="bg-nano-bg border border-nano-border rounded p-4 text-[11px] text-nano-accent overflow-x-auto leading-relaxed">
                    {card.diagram.trim()}
                  </pre>
                  <p className="mt-4 text-sm text-nano-text leading-relaxed">
                    {card.explanation}
                  </p>
                </div>
              )}
            </div>
          ))}
        </div>
      </div>
    </section>
  )
}
