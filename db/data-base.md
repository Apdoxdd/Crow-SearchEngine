## current config for database
    ### url frontier 
        - what the crawler saves to and what the indexer gets datafrom
        - saves:
                1) normalizedurl -> unqique key
                2) frequency -> how many times the link was mentioned
                4) isCrawled, isIndex
                5) document -> page raw html compressed
                6) hashedDoc_content -> to detect dublicates
                7) linkedPages -> the urls mentioned in that
                8) lastCrawled -> last time it was crawled (mby yes or no)
                9) pageRank -> to determine the rank of this page (mby yes or mby no)

    ### pages 
        - what the indexer writes to 
        - saves:
                1) id -> sha25 of the url 
                2) title 
                3) url 
                4) content -> pages body text 
                5) page_content_count -> number of meaningful words after cleaning
                6) pageRank ?

    ### words
        - inverted index saved from indexer 
        - saves:
                1) One record per stemmed word, listing every page containing it, with positions, TF and title/h1/h2 counts.



    PAGES
        +---------------+
        | id (PK)       |
        | title         |
        | url           |
        | content       |
        +---------------+
                |
                | 1
                |
                | Many
                |
        +---------------+
        |   POSTINGS    |
        +---------------+
        | page_id (FK)  |
        | term_id (FK)  |
        | tf            |
        | positions     |
        +---------------+
                |
                | Many
                |
                | 1
                |
        +---------------+
        | INVERTED_TREE |
        +---------------+
        | id (PK)       |
        | term          |
        +---------------+


        <BS>             SQLite Database File
                       |
             ┌─────────┼─────────┐
             |         |         |
          Crawler   Indexer   QueryEngine
          Manager   Manager    Manager
             |         |         |
          Conn 1    Conn 2    Conn 3
