<script lang="ts">
  import { onMount } from 'svelte'
  import {
    activeTab,
    playerState,
    queueStore,
    snapshot,
    backendOnline,
  } from './lib/stores'
  import { api, hasToken, setToken } from './lib/api'
  import { connectSnapshot } from './lib/ws'
  import Nav from './components/Nav.svelte'
  import NowPlaying from './pages/NowPlaying.svelte'
  import Library from './pages/Library.svelte'
  import Queue from './pages/Queue.svelte'
  import Pipeline from './pages/Pipeline.svelte'
  import { KeyRound } from 'lucide-svelte'

  let needToken = $state(false)
  let tokenInput = $state('')

  function saveToken() {
    setToken(tokenInput)
    needToken = false
  }

  function skipToken() {
    // Server with no auth (empty token) -- store empty so we don't ask again.
    setToken('')
    needToken = false
  }

  async function poll() {
    try {
      const [st, q] = await Promise.all([api.getState(), api.getQueue()])
      $playerState = st
      $queueStore = q
      $backendOnline = true
    } catch {
      $backendOnline = false
    }
  }

  onMount(() => {
    if (!hasToken()) {
      needToken = true
    }

    poll()
    const pollTimer = setInterval(poll, 1000)

    const disconnect = connectSnapshot((snap) => {
      $snapshot = snap
      $backendOnline = true
    })

    return () => {
      clearInterval(pollTimer)
      disconnect()
    }
  })
</script>

<div class="app-bg"></div>

<Nav />

<main class="min-h-0 flex-1">
  {#if $activeTab === 'nowplaying'}
    <NowPlaying />
  {:else if $activeTab === 'library'}
    <Library />
  {:else if $activeTab === 'queue'}
    <Queue />
  {:else if $activeTab === 'pipeline'}
    <Pipeline />
  {/if}
</main>

{#if needToken}
  <div
    class="fixed inset-0 z-[100] grid place-items-center bg-black/70 backdrop-blur-md"
  >
    <div class="glass-strong w-full max-w-md p-7">
      <div class="mb-4 flex items-center gap-3">
        <div
          class="grid h-11 w-11 place-items-center rounded-full bg-accent/20 text-accent"
        >
          <KeyRound size={20} />
        </div>
        <div>
          <h2 class="text-lg font-bold text-white">Access token</h2>
          <p class="text-xs text-white/50">
            Set in <span class="font-mono">[web] token</span> in the daemon
            config.
          </p>
        </div>
      </div>

      <input
        type="password"
        bind:value={tokenInput}
        placeholder="Bearer token"
        class="w-full rounded-xl border border-white/15 bg-white/[0.06] px-4 py-3 text-sm text-white placeholder:text-white/35 focus:border-accent/50 focus:outline-none"
        onkeydown={(e) => e.key === 'Enter' && saveToken()}
      />

      <div class="mt-5 flex gap-3">
        <button
          class="flex-1 rounded-xl bg-accent py-2.5 text-sm font-semibold text-black transition hover:brightness-110"
          onclick={saveToken}
        >
          Save
        </button>
        <button
          class="rounded-xl border border-white/15 px-5 py-2.5 text-sm text-white/70 transition hover:bg-white/10"
          onclick={skipToken}
        >
          No auth
        </button>
      </div>
    </div>
  </div>
{/if}
