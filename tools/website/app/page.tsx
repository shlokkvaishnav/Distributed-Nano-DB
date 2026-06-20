import Hero from '@/components/Hero'
import Playground from '@/components/Playground'
import Architecture from '@/components/Architecture'
import Header from '@/components/Header'

export default function Home() {
  return (
    <main className="min-h-screen">
      <Header />
      <Hero />
      <Playground />
      <Architecture />
    </main>
  )
}
