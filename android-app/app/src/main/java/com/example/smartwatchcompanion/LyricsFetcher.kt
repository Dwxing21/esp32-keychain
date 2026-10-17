package com.example.smartwatchcompanion

import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.text.Normalizer
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * Spotify has no public lyrics API, so this uses lrclib.net -- a free,
 * community-run database that returns synced (per-line timestamped) lyrics
 * where available. Search results are locally matched against title, artist,
 * and duration so a similarly named song is less likely to be selected.
 */
object LyricsFetcher {
    private const val TAG = "LyricsFetcher"

    data class LyricLine(val timeMs: Long, val text: String)

    /** Blocking network call -- run this off the main thread. */
    fun fetchSyncedLyrics(track: String, artist: String, durationSec: Int): List<LyricLine>? {
        if (track.isBlank()) return null
        val queryForms = linkedSetOf("$track $artist".trim(), "${greeklish(track)} ${greeklish(artist)}".trim())
        val expectedTrack = normalize(track)
        val expectedArtist = normalize(artist)
        val matches = mutableListOf<JSONObject>()
        fun addResults(queries: Set<String>) {
            for (query in queries) {
                val results = search(query) ?: continue
                for (i in 0 until results.length()) {
                    val candidate = results.optJSONObject(i) ?: continue
                    if (candidate.optString("syncedLyrics").isNotBlank()) matches.add(candidate)
                }
            }
        }
        fun bestCandidate(): JSONObject? = matches
            .distinctBy { it.optInt("id", it.toString().hashCode()) }
            .map { candidate -> candidate to score(candidate, expectedTrack, expectedArtist, durationSec) }
            .filter { (_, score) -> score.title >= 0.72 && (expectedArtist.isBlank() || score.artist >= 0.45) }
            .maxByOrNull { (_, score) -> score.total }
            ?.first

        addResults(queryForms)
        var best = bestCandidate()
        if (best == null) {
            addResults(linkedSetOf(track, greeklish(track)))
            best = bestCandidate()
        }
        best ?: return null
        return parseLrc(best.optString("syncedLyrics"))
            .takeIf { it.isNotEmpty() }
    }

    private data class MatchScore(val title: Double, val artist: Double, val total: Double)

    private fun search(query: String): JSONArray? {
        return try {
            val url = "https://lrclib.net/api/search?q=${URLEncoder.encode(query, "UTF-8")}"
            val conn = URL(url).openConnection() as HttpURLConnection
            conn.connectTimeout = 5000
            conn.readTimeout = 5000
            try {
                if (conn.responseCode != 200) return null
                JSONArray(conn.inputStream.bufferedReader().use { it.readText() })
            } finally {
                conn.disconnect()
            }
        } catch (e: Exception) {
            Log.w(TAG, "Lyrics search failed for \"$query\"", e)
            null
        }
    }

    private fun score(candidate: JSONObject, expectedTrack: String, expectedArtist: String, durationSec: Int): MatchScore {
        val titleScore = similarity(expectedTrack, normalize(candidate.optString("trackName")))
        val artistScore = if (expectedArtist.isBlank()) 1.0 else
            similarity(expectedArtist, normalize(candidate.optString("artistName")))
        val candidateDuration = candidate.optInt("duration", 0)
        val durationScore = if (durationSec <= 0 || candidateDuration <= 0) 0.0 else {
            val difference = kotlin.math.abs(candidateDuration - durationSec).toDouble()
            (1.0 - difference / maxOf(10.0, durationSec * 0.15)).coerceIn(0.0, 1.0)
        }
        return MatchScore(titleScore, artistScore, titleScore * 0.55 + artistScore * 0.30 + durationScore * 0.15)
    }

    private fun similarity(first: String, second: String): Double {
        if (first == second) return 1.0
        if (first.isBlank() || second.isBlank()) return 0.0
        val firstWords = first.split(' ').filter { it.isNotBlank() }.toSet()
        val secondWords = second.split(' ').filter { it.isNotBlank() }.toSet()
        val wordScore = firstWords.intersect(secondWords).size.toDouble() /
            firstWords.union(secondWords).size.coerceAtLeast(1)
        val editScore = 1.0 - levenshtein(first, second).toDouble() / maxOf(first.length, second.length)
        return maxOf(wordScore, editScore)
    }

    private fun levenshtein(first: String, second: String): Int {
        var previous = IntArray(second.length + 1) { it }
        for (i in first.indices) {
            val current = IntArray(second.length + 1)
            current[0] = i + 1
            for (j in second.indices) {
                current[j + 1] = minOf(
                    current[j] + 1,
                    previous[j + 1] + 1,
                    previous[j] + if (first[i] == second[j]) 0 else 1
                )
            }
            previous = current
        }
        return previous[second.length]
    }

    private fun normalize(value: String): String = greeklish(
        Normalizer.normalize(value, Normalizer.Form.NFD).replace(Regex("\\p{M}+"), "")
    )
        .lowercase()
        .replace(Regex("[^\\p{L}\\p{N}]+"), " ")
        .trim()
        .replace(Regex("\\s+"), " ")

    fun toDisplayText(value: String): String = greeklish(value)

    private fun greeklish(value: String): String {
        val unaccented = Normalizer.normalize(value, Normalizer.Form.NFD)
            .replace(Regex("\\p{M}+"), "")
        return buildString(unaccented.length) {
            for (character in unaccented.lowercase()) {
                append(
                    when (character) {
                        'α' -> "a"; 'β' -> "v"; 'γ' -> "g"; 'δ' -> "d"; 'ε' -> "e"
                        'ζ' -> "z"; 'η' -> "i"; 'θ' -> "th"; 'ι' -> "i"; 'κ' -> "k"
                        'λ' -> "l"; 'μ' -> "m"; 'ν' -> "n"; 'ξ' -> "x"; 'ο' -> "o"
                        'π' -> "p"; 'ρ' -> "r"; 'σ', 'ς' -> "s"; 'τ' -> "t"; 'υ' -> "y"
                        'φ' -> "f"; 'χ' -> "ch"; 'ψ' -> "ps"; 'ω' -> "o"
                        else -> character.toString()
                    }
                )
            }
        }
    }

    /** Parses "[mm:ss.xx]lyric text" lines (LRC format) into timed lines. */
    private fun parseLrc(lrc: String): List<LyricLine> {
        val tagPattern = Regex("""\[(\d+):(\d+(?:\.\d+)?)]""")
        val lines = mutableListOf<LyricLine>()
        for (rawLine in lrc.lines()) {
            val matches = tagPattern.findAll(rawLine).toList()
            if (matches.isEmpty()) continue
            val text = rawLine.replace(tagPattern, "").trim()
            if (text.isEmpty()) continue
            for (m in matches) {
                val minutes = m.groupValues[1].toLong()
                val seconds = m.groupValues[2].toDouble()
                val timeMs = minutes * 60_000 + (seconds * 1000).toLong()
                lines.add(LyricLine(timeMs, text))
            }
        }
        return lines.sortedBy { it.timeMs }
    }
}
