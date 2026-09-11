/*++

Module Name:

    cable.h

Abstract:

    Общий кольцевой буфер между render-endpoint (Speakers) и capture-endpoint
    (Microphone) виртуального кабеля. Render-поток кладёт байты из своего
    WaveRT-буфера (ReadBytes), capture-поток забирает их в свой (WriteBytes).
    Оба endpoint'а работают в одном формате (48 kHz, 2 ch, 16-bit PCM),
    поэтому буфер байтовый, без конвертации.

    Чтение начинается только после накопления primeBytes (задержка кабеля);
    при опустошении capture получает тишину и снова ждёт накопления.
    Переполнение отбрасывает самые старые данные.

--*/

#ifndef _BOMBOEC_CABLE_H_
#define _BOMBOEC_CABLE_H_

class CCableBuffer
{
public:
    NTSTATUS Init(_In_ ULONG capacityBytes, _In_ ULONG primeBytes);
    void Free();

    // Producer (render stream). Вызывается под spinlock позиции потока,
    // IRQL <= DISPATCH_LEVEL.
    void Write(_In_reads_bytes_(bytes) const BYTE* data, _In_ ULONG bytes);

    // Consumer (capture stream). Всегда заполняет все bytes (данные или тишина).
    void Read(_Out_writes_bytes_all_(bytes) BYTE* data, _In_ ULONG bytes);

    ULONG Available() const { return m_count; }

private:
    KSPIN_LOCK  m_lock;
    BYTE*       m_buffer;
    ULONG       m_capacity;
    ULONG       m_prime;
    ULONG       m_head;     // индекс записи
    ULONG       m_count;    // доступно байт
    BOOLEAN     m_primed;
    ULONGLONG   m_underruns;
    ULONGLONG   m_overruns;
};

extern CCableBuffer g_Cable;

#endif // _BOMBOEC_CABLE_H_
