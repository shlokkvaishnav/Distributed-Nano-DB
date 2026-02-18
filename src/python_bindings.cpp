#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "../include/core/hnsw.hpp"
#include "../include/core/quantizer.hpp"

namespace py = pybind11;
using namespace nanodb;

PYBIND11_MODULE(nanodb, m) {
    m.doc() = "NanoDB: High-Performance Vector Search Engine (C++ Backend)";

    // ---- DistanceMetric enum ----
    py::enum_<DistanceMetric>(m, "DistanceMetric")
        .value("L2",           DistanceMetric::L2,
               "Squared Euclidean distance (default). Fast, no sqrt. Use for general-purpose ANN.")
        .value("Cosine",       DistanceMetric::Cosine,
               "1 - cosine_similarity. Use for NLP embeddings (BERT, OpenAI, etc.).")
        .value("InnerProduct", DistanceMetric::InnerProduct,
               "Negative dot product. Use for recommendation systems or pre-normalized vectors.")
        .export_values();

    // ---- MMapHandler ----
    py::class_<MMapHandler>(m, "MMapHandler")
        .def(py::init<>())
        .def("open_file",  &MMapHandler::open_file)
        .def("close_file", &MMapHandler::close_file);

    // ---- Result ----
    py::class_<Result>(m, "Result")
        .def_readonly("id",       &Result::id)
        .def_readonly("distance", &Result::distance)
        .def_readwrite("metadata",&Result::metadata)
        .def("__repr__", [](const Result& r) {
            return "<Result id=" + std::to_string(r.id) +
                   " dist=" + std::to_string(r.distance) +
                   " meta='" + r.metadata + "'>";
        });

    // ---- HNSW ----
    py::class_<HNSW>(m, "HNSW")
        .def(py::init<MMapHandler&, std::string, DistanceMetric>(),
             py::arg("storage"),
             py::arg("meta_path") = "data/metadata.bin",
             py::arg("metric")    = DistanceMetric::L2)

        .def("insert", &HNSW::insert,
             "Insert a vector with an integer ID and optional metadata string.",
             py::arg("vector"), py::arg("id"), py::arg("metadata") = "",
             py::call_guard<py::gil_scoped_release>())

        .def("search", &HNSW::search,
             "Search for k nearest neighbors. Returns a list of Result objects.",
             py::arg("query"), py::arg("k") = 5)

        .def("delete_vector", &HNSW::delete_vector,
             "Lazily delete a vector by ID (tombstone). It will no longer appear in search results.\n"
             "Note: the node remains in the graph structure; periodic index rebuilds are needed\n"
             "to reclaim space and restore full recall after many deletions.",
             py::arg("id"))

        .def("is_deleted",   &HNSW::is_deleted,
             "Returns True if the given ID has been deleted.", py::arg("id"))

        .def("get_metadata", &HNSW::get_metadata,
             "Retrieve the metadata string for a given ID.", py::arg("id"))

        .def("size",   &HNSW::size,   "Total number of inserted elements (including deleted).")
        .def("metric", &HNSW::metric, "The DistanceMetric this index was built with.");

    // ---- ScalarQuantizer ----
    py::class_<ScalarQuantizer>(m, "ScalarQuantizer",
        "int8 Scalar Quantizer: 4x memory reduction with ~1-5% recall loss.\n"
        "Usage: train() on a sample, then quantize() before inserting into a compressed index.")
        .def(py::init<>())

        .def("train", [](ScalarQuantizer& q, const std::vector<std::vector<float>>& vecs) {
            q.train(vecs);
        }, "Train on a list of float vectors to compute per-dimension min/max ranges.",
           py::arg("vectors"))

        .def("quantize", [](const ScalarQuantizer& q, const std::vector<float>& vec) {
            std::vector<int8_t> out(vec.size());
            q.quantize(vec.data(), out.data(), vec.size());
            return out;
        }, "Quantize a float32 vector to int8. Returns a list of int8 values.",
           py::arg("vector"))

        .def("dequantize", [](const ScalarQuantizer& q, const std::vector<int8_t>& vec, size_t dim) {
            std::vector<float> out(dim);
            q.dequantize(vec.data(), out.data(), dim);
            return out;
        }, "Dequantize an int8 vector back to approximate float32.",
           py::arg("vector"), py::arg("dim"))

        .def("is_trained", &ScalarQuantizer::is_trained)
        .def("dim",        &ScalarQuantizer::dim);
}