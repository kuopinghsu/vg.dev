# OpenVG 1.1 Hardware Accelerator Specification & C-Model Implementation

This document details the architectural design and C-model implementation steps for an OpenVG 1.1 hardware accelerator. The design is split into a **Front-End (FE)** and a **Back-End (BE)** to optimize for throughput and memory bandwidth.

---

## 1. System Architecture Overview

### A. Front-End (Geometry Engine)
The Front-End is responsible for high-level path processing and transforming vector data into raster-ready primitives.

1.  **Curve Flattening**:
    -   OpenVG paths consist of line segments, quadratic Beziers, and cubic Beziers.
    -   **Algorithm**: Use **Recursive De Casteljau's subdivision**. 
    -   **Stop Condition**: Subdivide until the deviation from a straight line is less than a sub-pixel threshold (e.g., 0.25 pixels).
2.  **Coordinate Transformation**:
    -   Apply the Affine Transformation Matrix (User-to-Surface) to all flattened vertices.
3.  **Fill Rule Logic (Triangle/Trapezoid Generation)**:
    -   Convert the closed path into a list of directed line segments (edges).
    -   **Non-Zero Rule**: Hardware maintains a winding counter.
    -   **Even-Odd Rule**: Hardware toggles a parity bit.

### B. Back-End (Rasterization Engine)
The Back-End uses a **Tile-Based Architecture** to minimize external memory traffic.

1.  **Tiling (Binning)**:
    -   The screen is divided into $32 \times 32$ or $64 \times 64$ pixel tiles.
    -   The FE outputs edges to a **Global Edge Buffer**.
    -   The Tiler assigns each edge to a **Tile List** for every tile it intersects.
2.  **Scanline Rasterizer**:
    -   Processes one tile at a time.
    -   For each scanline within the tile, it calculates the X-intersections of all edges in the Tile List.
3.  **Anti-Aliasing (AA)**:
    -   Uses a **Sub-pixel Mask** approach (e.g., $8 \times 8$ sampling).
    -   Determines the coverage of each pixel by checking which sub-pixels are "inside" the path based on the fill rule.

---

## 2. 4-Way Set Associative Texture Cache

To speed up image/gradient sampling, a hardware cache is used to interface with the VRAM.

### Design Parameters
-   **Cache Line Size**: 64 Bytes (typically 16 pixels at 32bpp).
-   **Associativity**: 4-Way Set Associative.
-   **Replacement Policy**: Least Recently Used (LRU).

### C-Model Implementation Logic
1.  **Address Decomposition**:
    -   `Offset`: `addr & 0x3F` (Bits 0-5)
    -   `Index`: `(addr >> 6) & (num_sets - 1)`
    -   `Tag`: `addr >> (6 + index_bits)`
2.  **Lookup Process**:
    -   Calculate `index`. 
    -   Compare `Tag` against the 4 slots in `cache[index]`.
    -   If `hit`: Update LRU status to move this slot to "Most Recently Used."
    -   If `miss`: 
        -   Identify the slot with the lowest LRU value.
        -   Fetch 64 bytes from memory into that slot.
        -   Update the Tag and reset LRU bits.

---

## 3. Tile-Based Scanline with AA Algorithm

This algorithm is the core of the Back-End efficiency.

### Step-by-Step Implementation:
1.  **Binning**: Iterate through all flattened edges. For each edge, calculate which tiles $(T_x, T_y)$ its bounding box touches. Append the edge ID to the corresponding Tile Lists.
2.  **Tile Loop**: Process tiles sequentially.
    -   **Clear Tile Buffer**: Initialize a local SRAM buffer representing the tile.
    -   **Active Edge Table (AET)**: For each scanline $Y$:
        -   Find edges where $Y_{min} \le Y < Y_{max}$.
        -   Calculate $X$ intersections using the slope: $x = x_{start} + (1/m) \cdot (Y - y_{start})$.
        -   Sort intersections by $X$.
    -   **Fill & AA**:
        -   For each pixel on the scanline, generate an $N \times N$ sub-pixel grid.
        -   For each sub-pixel $(px, py)$, apply the **Fill Rule** against the edges in the AET.
        -   The **Coverage Value** for the pixel is: $\frac{\text{Set Sub-pixels}}{N^2}$.
    -   **Shading**:
        -   Sample the **Texture Cache** using transformed $(u, v)$ coordinates.
        -   Apply the `Coverage Value` to the Alpha channel.
        -   Perform Porter-Duff blending with the Tile Buffer.
3.  **Write-Back**: Once the tile is fully processed, burst the Tile Buffer from SRAM to the Frame Buffer in main memory.

---

## 4. C-Model Implementation Roadmap

1.  **Primitive Setup**: Define `struct Edge { float x0, y0, x1, y1; int direction; };`.
2.  **FE Loop**: Flatten curves into `std::vector<Edge>`.
3.  **Tiling**: Create `std::vector<int> tile_buckets[MAX_TILES]`.
4.  **Rasterization**: 
    -   Implement `compute_coverage(pixel_x, pixel_y, tile_edges)`.
    -   Implement `texture_cache_read(addr)`.
5.  **Output**: Write the final pixel array to a PPM or PNG file for visual verification.
