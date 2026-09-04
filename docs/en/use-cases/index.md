# Use cases

Four guides, each built around one thing nvecd does. Every command in them was run against a live server and shows the response the server actually sent, and each guide says where the use case runs into something nvecd does not do.

| Guide | What it teaches |
|---|---|
| [Recommendations](./recommendations.md) | A recommender built from interaction events alone, with no vectors and no embedding model. |
| [E-commerce](./e-commerce.md) | Engagement-weighted events, product embeddings, `using=fusion` with adaptive weighting, and metadata filters. |
| [Real-time feed](./real-time-feed.md) | A high-rate engagement stream with a short half-life, negative signals, and the settings that bound memory. |
| [Semantic search](./semantic-search.md) | `SIMV` retrieval from a query vector over a document corpus, the ANN index choice, and recall at corpus scale. |

Read [Recommendations](./recommendations.md) first. It is the smallest complete system, and the other three build on its vocabulary.
