#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "VideoCommon/MOHFrontline/Assets/GC/Formats/GCCompartment.h"

namespace Viv = MOHFrontline::GCViv;
namespace World = MOHFrontline::GCCompartment;

void Put(std::vector<unsigned char>& b, size_t at, uint32_t v, unsigned width = 4)
{
  for (unsigned i = 0; i < width; ++i)
    b[at + i] = v >> (8 * (width - i - 1));
}

int main(int argc, char** argv)
{
  // A tuple contains four independent indices, not interleaved vertex data.
  // The 16-bit fixture deliberately addresses an element beyond 255.
  for (unsigned width : {1u, 2u})
  {
    std::vector<unsigned char> b(0x4000);
    Put(b, 0, 17); Put(b, 4, b.size());
    Put(b, 8, 0x3000); Put(b, 12, 1);
    Put(b, 24, 0x3200); Put(b, 28, 1);
    Put(b, 32, 0x3100); Put(b, 36, 1);
    Put(b, 0x3200, 0x3100); Put(b, 0x3208, 0x3000); Put(b, 0x320c, 0x40);
    Put(b, 0x40, width - 1, 2); Put(b, 0x42, 1, 2);
    Put(b, 0x44, 0x200); Put(b, 0x48, 0x1800);
    Put(b, 0x4c, 0x1200); Put(b, 0x50, 0x2000); Put(b, 0x54, 0x80);
    const unsigned index = width == 1 ? 1 : 256;
    for (unsigned i = 0; i < 4; ++i)
      Put(b, 0xb4 + i * width, index, width);
    Put(b, 0x200 + index * 12, std::bit_cast<uint32_t>(1.25f));
    Put(b, 0x204 + index * 12, std::bit_cast<uint32_t>(-2.5f));
    Put(b, 0x208 + index * 12, std::bit_cast<uint32_t>(3.0f));
    b[0x1200 + index * 3] = 0xc0; // GX signed normal: -64 / 64 = -1
    Put(b, 0x1800 + index * 4, 0x11223344);
    Put(b, 0x2000 + index * 8, std::bit_cast<uint32_t>(-0.5f));
    Put(b, 0x2004 + index * 8, std::bit_cast<uint32_t>(0.75f));
    // The shape offset is read from SHPG + 0x14, not assumed to be +0x30.
    Put(b, 0x3060, 0x3300); Put(b, 0x3300, 0x53485047); Put(b, 0x3314, 0x50);
    auto cpt = World::CPT::Parse(b);
    assert(cpt && cpt->Geometry(0) == 0x40 && cpt->Shape(0, 0) == 0x3350);
    World::Vertex v;
    assert(cpt->DecodeVertex(0x40, 0, &v));
    assert((v.position == std::array<float, 3>{1.25f, -2.5f, 3.0f}));
    assert((v.normal == std::array<float, 3>{-1.0f, 0.0f, 0.0f}));
    assert((v.color == std::array<unsigned char, 4>{0x11, 0x22, 0x33, 0x44}));
    assert((v.uv == std::array<float, 2>{-0.5f, 0.75f}));
    assert(!cpt->DecodeVertex(0x40, 1, &v));
    Put(b, 0x44, 0xfffffff0);
    assert(!cpt->DecodeVertex(0x40, 0, &v));
  }
  {
    std::vector<unsigned char> b(0x100);
    Put(b, 0, 7); Put(b, 4, b.size());
    Put(b, 8, 3); Put(b, 12, 0x74); Put(b, 16, 1); Put(b, 36, 0xc0);
    for (unsigned i = 0; i < 3; ++i)
    {
      Put(b, 0xc0 + i * 4, 0x74 + i * 12);
      Put(b, 0x74 + i * 12, std::bit_cast<uint32_t>(float(i + 1)));
    }
    auto cdb = World::CDB::Parse(b);
    std::array<std::array<float, 3>, 3> triangle;
    assert(cdb && cdb->DecodeTriangle(0, &triangle));
    assert(triangle[0][0] == 1 && triangle[1][0] == 2 && triangle[2][0] == 3);
    Put(b, 0xc0, 0x75); // In-range but not an actual vector record.
    assert(!cdb->DecodeTriangle(0, &triangle));
  }
  // Same payload in each directory encoding, including an offset above 16 MiB.
  for (bool big : {false, true})
  {
    const unsigned width = big ? 4 : 3;
    const size_t start = big ? 16 : 6;
    std::vector<unsigned char> b(start + width * 2 + 2);
    if (big)
    {
      b[0] = 'B'; b[1] = 'I'; b[2] = 'G'; b[3] = 'F';
      Put(b, 8, 1); Put(b, 12, b.size());
    }
    else
    {
      b[0] = 0xc0; b[1] = 0xfb; Put(b, 4, 1, 2);
    }
    const uint32_t offset = big ? 0x1000040 : 0x40;
    Put(b, start, offset, width); Put(b, start + width, 8, width);
    b[start + width * 2] = 'x';
    auto parsed = Viv::Parse(b, uint64_t(offset) + 8);
    assert(parsed && parsed->size() == 1 && parsed->front().offset == offset);
    assert(!Viv::Parse(b, uint64_t(offset) + 7));
    auto bad = b; bad.back() = 'x';
    assert(!Viv::Parse(bad, uint64_t(offset) + 8));
    bad = b; Put(bad, start, 1, width);
    assert(!Viv::Parse(bad, uint64_t(offset) + 8));
    for (size_t n = 0; n < b.size(); ++n)
      assert(!Viv::Parse(std::span(b).first(n), uint64_t(offset) + 8));
  }

  assert(!World::CPT::Parse({}));
  assert(!World::CDB::Parse({}));
  size_t archives = 0, cpts = 0, cdbs = 0, vertices = 0, triangles = 0, wide = 0;
  if (argc == 2)
  {
    for (const auto& file : std::filesystem::recursive_directory_iterator(argv[1]))
    {
      if (file.path().extension() != ".viv")
        continue;
      std::ifstream input(file.path(), std::ios::binary);
      std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), {}};
      auto entries = Viv::Parse(bytes, bytes.size());
      if (!entries) { std::cerr << "VIV rejected: " << file.path() << '\n'; return 1; }
      ++archives;
      for (const auto& entry : *entries)
      {
        auto data = std::span(bytes).subspan(entry.offset, entry.size);
        if (entry.name.ends_with(".cpt"))
        {
          auto cpt = World::CPT::Parse(data);
          if (!cpt) { std::cerr << "CPT rejected: " << entry.name << '\n'; return 1; }
          ++cpts;
          for (uint32_t i = 0; i < cpt->tables[2].count; ++i)
          {
            auto g = cpt->Geometry(i);
            if (!g) { std::cerr << "chunk rejected: " << entry.name << ':' << i << '\n'; return 1; }
            wide += cpt->data.U16(*g) & 1;
            for (uint32_t j = 0; j < cpt->data.U16(*g + 2); ++j)
            {
              World::Vertex vertex;
              if (!cpt->DecodeVertex(*g, j, &vertex))
              { std::cerr << "vertex rejected: " << entry.name << ':' << i << ':' << j << '\n'; return 1; }
              ++vertices;
            }
          }
          auto damaged = std::vector<unsigned char>(data.begin(), data.end());
          Put(damaged, 8, 0xfffffff0); Put(damaged, 12, 8);
          assert(!World::CPT::Parse(damaged));
        }
        else if (entry.name.ends_with(".cdb"))
        {
          auto cdb = World::CDB::Parse(data);
          if (!cdb) { std::cerr << "CDB rejected: " << entry.name << '\n'; return 1; }
          ++cdbs;
          for (uint32_t i = 0; i < cdb->triangles.count; ++i)
          {
            std::array<std::array<float, 3>, 3> triangle;
            if (!cdb->DecodeTriangle(i, &triangle))
            { std::cerr << "triangle rejected: " << entry.name << ':' << i << '\n'; return 1; }
            ++triangles;
          }
        }
      }
    }
  }
  std::cout << "PASS archives=" << archives << " CPT=" << cpts << " CDB=" << cdbs
            << " vertices=" << vertices << " triangles=" << triangles << " wide-index-chunks=" << wide << '\n';
}
