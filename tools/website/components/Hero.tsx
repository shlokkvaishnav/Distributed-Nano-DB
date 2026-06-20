'use client'

export default function Hero() {
  return (
    <section className="pt-32 pb-20 px-6">
      <div className="max-w-4xl mx-auto text-center">
        <h1 className="text-4xl md:text-5xl font-bold tracking-tight mb-4">
          <span className="text-nano-accent">NanoDB</span>
          <span className="text-nano-text">: A Vector Search Engine</span>
          <br />
          <span className="text-nano-muted text-2xl md:text-3xl font-medium">
            Built from Scratch in C++
          </span>
        </h1>

        <p className="text-nano-muted text-lg mt-6 max-w-2xl mx-auto">
          Sub-millisecond ANN search. Zero external dependencies. Runs in Docker.
        </p>

        <div className="flex items-center justify-center gap-12 mt-12">
          <div className="text-center">
            <div className="text-3xl md:text-4xl font-bold text-nano-accent stat-glow">
              6,500
            </div>
            <div className="text-xs text-nano-muted mt-1 uppercase tracking-wider">
              TPS @ 8 threads
            </div>
          </div>
          <div className="w-px h-12 bg-nano-border" />
          <div className="text-center">
            <div className="text-3xl md:text-4xl font-bold text-nano-accent stat-glow">
              0.15ms
            </div>
            <div className="text-xs text-nano-muted mt-1 uppercase tracking-wider">
              Search latency
            </div>
          </div>
          <div className="w-px h-12 bg-nano-border" />
          <div className="text-center">
            <div className="text-3xl md:text-4xl font-bold text-nano-accent stat-glow">
              &ge;95%
            </div>
            <div className="text-xs text-nano-muted mt-1 uppercase tracking-wider">
              Recall@10
            </div>
          </div>
        </div>

        <div className="flex items-center justify-center gap-4 mt-10">
          <a
            href="#playground"
            className="px-6 py-2.5 bg-nano-accent text-nano-bg font-semibold rounded-md hover:bg-nano-accent/90 transition-colors text-sm"
          >
            Try it Live
          </a>
          <a
            href="https://github.com/shlokkvaishnav/nano-db"
            target="_blank"
            rel="noopener noreferrer"
            className="px-6 py-2.5 border border-nano-border text-nano-text rounded-md hover:border-nano-accent/50 transition-colors text-sm"
          >
            View on GitHub
          </a>
        </div>
      </div>
    </section>
  )
}
